// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_METADATA_H_
#define BESS_METADATA_H_

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "snbuf_layout.h"

class Module;

namespace bess {
class PacketRef;
}

namespace bess {
namespace metadata {

// In bytes, per attribute.
static const size_t kMetadataAttrMaxSize = 32;
static_assert(kMetadataAttrMaxSize <= SIZE_MAX,
              "Max metadata attr size check failed");

// Max number of attributes per module.
static const size_t kMaxAttrsPerModule = 16;
static_assert(kMaxAttrsPerModule <= SIZE_MAX,
              "Max metadata attrs per module check failed");

// Total size of all metadata in bytes.
static const size_t kMetadataTotalSize = SNBUF_METADATA;
static_assert(kMetadataTotalSize <= SIZE_MAX,
              "Total metadata size check failed");

// Normal offset values are 0 or a positive value.
typedef int8_t mt_offset_t;
typedef int16_t scope_id_t;

// No downstream module reads the attribute, so the module can skip writing.
static const mt_offset_t kMetadataOffsetNoWrite = -1;

// No upstream module writes the attribute, thus garbage value will be read.
static const mt_offset_t kMetadataOffsetNoRead = -2;

// Out of space in packet buffers for the attribute.
static const mt_offset_t kMetadataOffsetNoSpace = -3;

// The attribute has no writer, and its readers declared a default (Contract::
// kDefault). The bytes in the packet are never read: packet access goes through
// Module::ResolveAttr()/ResolvedAttr<T>::Get(), which returns the declared
// default. Like the other negative values, IsValidOffset() is false.
static const mt_offset_t kMetadataOffsetDefault = -4;

// A writer whose attribute is read downstream, but whose reader is also
// reachable from a path with no writer (an invalid scope): the reader gets
// kMetadataOffsetNoRead (or its default), so this write is skipped too. Unlike
// kMetadataOffsetNoWrite, a reader exists: a writer that must place the value
// for its reader (a classifier's result field) fails closed on it.
static const mt_offset_t kMetadataOffsetInvalidScope = -5;

static inline int IsValidOffset(mt_offset_t offset) {
  return (offset >= 0);
}

struct Attribute {
  // What a reader may assume about an attribute that the graph does not write.
  // Orthogonal to AccessMode: the contract is a statement by a reader, and a
  // writer's contract is ignored (it is the provider, not the consumer).
  enum class Contract {
    // Unresolved is allowed; a reader sees kMetadataOffsetNoRead and decides
    // what to do (the historical behavior).
    kBestEffort = 0,

    // Resume fails, naming this module and attribute, unless some upstream
    // module writes the attribute on every path that reaches the reader.
    kRequired,

    // A provider-less reader resolves to kMetadataOffsetDefault and reads
    // `default_value` through ResolvedAttr<T>::Get(), at no packet-path cost:
    // the packet's metadata bytes are not touched. A provider that appears in
    // a later graph replaces the default at the next resume.
    kDefault,
  };

  Attribute()
      : name(),
        size(),
        mode(),
        scope_id(),
        contract(Contract::kBestEffort),
        default_value() {}

  std::string name;
  size_t size;  // in bytes
  enum class AccessMode { kRead = 0, kWrite, kUpdate } mode;
  mutable int scope_id;
  Contract contract;
  // Valid only when contract == Contract::kDefault; the first `size` bytes.
  std::array<uint8_t, kMetadataAttrMaxSize> default_value;
};

// A read side resolved at PreResume: either a packet-metadata offset, or the
// attribute's declared default when the graph has no provider. Resolve it once
// per batch (Module::ResolveAttr<T>) and call Get() per packet; the common
// valid-offset path is the same load it was before, and the default path does
// not touch packet memory.
template <typename T>
struct ResolvedAttr {
  mt_offset_t offset = kMetadataOffsetNoRead;
  T default_value{};
  bool has_default = false;

  bool resolved() const { return IsValidOffset(offset); }

  // Defined in module.h, where packet access lives. The valid-offset path is
  // get_attr_with_offset<T>(); the default path returns default_value without
  // touching the packet.
  T Get(bess::PacketRef pkt) const;
};

// What the last metadata computation saw, by category. Built only while
// workers are paused (resume/apply); Report() is a read-only accessor, so
// GetPipeline can return it without walking the graph again.
struct MetadataDeclaration {
  std::string module;
  std::string attribute;
  size_t size = 0;
  Attribute::AccessMode mode = Attribute::AccessMode::kRead;
  Attribute::Contract contract = Attribute::Contract::kBestEffort;
  std::vector<uint8_t> default_value;  // `size` bytes for Contract::kDefault
};

struct MetadataConflict {
  std::string attribute;
  std::string reason;
  std::vector<std::string> modules;
};

struct MetadataReport {
  // Every attribute declaration the graph has, and how many of them resolved
  // to a usable packet offset. The control surface prints these when no
  // category below has an entry (a clean graph).
  size_t declarations = 0;
  size_t resolved = 0;

  // A writer whose value nothing downstream reads: the write is skipped
  // (kMetadataOffsetNoWrite).
  std::vector<MetadataDeclaration> writers_without_reader;

  // A read/update declaration with no upstream writer on every path. Includes
  // required declarations; see unsatisfied_required for the ones that failed.
  std::vector<MetadataDeclaration> readers_without_writer;

  // Required declarations whose reader got no provider; resume fails for
  // these.
  std::vector<MetadataDeclaration> unsatisfied_required;

  // Default declarations that resolved to kMetadataOffsetDefault.
  std::vector<MetadataDeclaration> defaults_applied;

  // Declarations of one name that disagree (contract or default bytes), or a
  // reader's attribute declared by more than one writer scope.
  // RegisterAttribute already refuses a size disagreement when a module is
  // created, so that class cannot reach a computed report.
  std::vector<MetadataConflict> conflicts;
};

typedef std::string attr_id_t;

class ScopeComponent {
 public:
  ScopeComponent()
      : attr_id_(),
        size_(),
        offset_(),
        scope_id_(),
        assigned_(),
        invalid_(),
        modules_(),
        degree_() {}

  ~ScopeComponent() {}

  const attr_id_t &attr_id() const { return attr_id_; }
  void set_attr_id(attr_id_t id) { attr_id_ = id; }

  int size() const { return size_; }
  void set_size(int size) { size_ = size; }

  mt_offset_t offset() const { return offset_; }
  void set_offset(mt_offset_t offset) { offset_ = offset; }

  scope_id_t scope_id() const { return scope_id_; }
  void set_scope_id(scope_id_t id) { scope_id_ = id; }

  bool assigned() const { return assigned_; }
  void set_assigned(bool assigned) { assigned_ = assigned; }

  bool invalid() const { return invalid_; }
  void set_invalid(bool invalid) { invalid_ = invalid; }

  const std::set<Module *> &modules() const { return modules_; }
  void add_module(Module *m) { modules_.insert(m); }
  void clear_modules() { modules_.clear(); }

  int degree() const { return degree_; }
  void incr_degree() { degree_++; }

  bool DisjointFrom(const ScopeComponent &rhs);

 private:
  /* identification fields */
  attr_id_t attr_id_;
  int size_;
  mt_offset_t offset_;
  scope_id_t scope_id_;

  /* computation state fields */
  bool assigned_;
  bool invalid_;
  std::set<Module *> modules_;
  int degree_;
};

class Pipeline {
 public:
  Pipeline()
      : scope_components_(),
        module_scopes_(),
        module_components_(),
        registered_attrs_() {}

  // Main entry point for calculating metadata offsets.
  // Returns 0 on success, -EINVAL if a required contract has no provider.
  int ComputeMetadataOffsets();

  // The categories of the last ComputeMetadataOffsets() call. Read-only: it
  // does not walk the graph or touch offsets, so the control plane can expose
  // it while workers run.
  const MetadataReport &report() const { return report_; }

  // The first unsatisfied required declaration, or nullptr. ComputeMetadata
  // Offsets() returns -EINVAL exactly when this is non-null.
  const MetadataDeclaration *first_unsatisfied_required() const {
    return report_.unsatisfied_required.empty()
               ? nullptr
               : &report_.unsatisfied_required.front();
  }

  // Registers attr and returns 0 if no attribute named @attr_name with size
  // other than @size has already been registered for this pipeline.
  // Returns -EINVAL on error.
  int RegisterAttribute(const std::string &attr_name, size_t size);
  void DeregisterAttribute(const std::string &attr_name);

 private:
  friend class MetadataTest;

  // Allocate and initiliaze scope component storage.
  // Returns 0 on sucess, -errno on failure.
  int PrepareMetadataComputation();

  void CleanupMetadataComputation();

  // Debugging tool.
  void LogAllScopes() const;

  // Add a module to the current scope component.
  void AddModuleToComponent(Module *m, const struct Attribute *attr);

  // Returns a pointer to an attribute if it's contained within a module.
  const struct Attribute *FindAttr(Module *m,
                                   const struct Attribute *attr) const;

  // Traverses module graph upstream to help identify a scope component.
  void TraverseUpstream(Module *m, const struct Attribute *attr);

  // Traverses module graph downstream to help identify a scope component.
  // Returns 0 if module is part of the scope component, -1 if not.
  int TraverseDownstream(Module *m, const struct Attribute *attr);

  // Wrapper for identifying scope components.
  void IdentifySingleScopeComponent(Module *m, const struct Attribute *attr);

  // Given a module that writes an attr, identifies the corresponding scope
  // component.
  void IdentifyScopeComponent(Module *m, const struct Attribute *attr);

  void FillOffsetArrays();
  void AssignOffsets();
  void ComputeScopeDegrees();

  // Applies default contracts to readers the graph did not resolve, then
  // fills report_ from the assigned offsets and declarations.
  void ApplyContractsAndBuildReport();

  std::vector<ScopeComponent> scope_components_;

  // Maps modules to the
  std::map<const Module *, scope_id_t> module_scopes_;

  // Maps modules to the scope componenets they belong to.
  std::map<const Module *, scope_id_t *> module_components_;

  // Keeps track of the attributes used by modules in this pipeline
  // count(=int) represents how many modules registered the attribute, and the
  // attribute is deregistered once it reaches back to 0.
  // Those modules should agree on the same size(=size_t).
  std::map<std::string, std::tuple<size_t, int> > registered_attrs_;

  // Last computed report; empty until ComputeMetadataOffsets() runs.
  MetadataReport report_;
};

extern bess::metadata::Pipeline default_pipeline;

}  // namespace metadata
}  // namespace bess

#endif  // BESS_METADATA_H_
