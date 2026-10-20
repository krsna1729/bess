// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_HASHLB_H_
#define BESS_MODULES_HASHLB_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "classifier/extract_plan.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

// Splits packets across output gates by a hash of L2/L3/L4 or chosen fields.
//
// The configuration (mode, gate list, field layout) is one immutable object
// published through an RcuPtr: the "<name>/settings" resource (HashLBSettings,
// D-129) builds a replacement and publishes it while workers keep processing
// (G1.2 mode G; the configuration is small and changes rarely). A batch reads
// it once.
// Decision D-017 (docs/decisions.md)
class HashLB final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  // "settings" (HashLBSettings): mode, fields and gates, applied live by an
  // RCU swap (D-129).
  static const ResourceTables resource_tables;

  HashLB() : Module(), config_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::HashLBArg &arg);
  void DeInit() override;

  std::string GetDesc() const override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  enum class Mode { kL2, kL3, kL4, kOther };
  static constexpr Mode kDefaultMode = Mode::kL4;
  static constexpr size_t kMaxGates = 16384;
  // The custom tuple's limits, the ones the field table this mode used to
  // build enforced (MAX_FIELDS and MAX_FIELD_SIZE there).
  static constexpr int kMaxFields = 8;
  static constexpr int kMaxFieldSize = 8;
  static constexpr int kMaxFieldOffset = 1024;
  // The widest key those limits allow: the packet path's key scratch.
  static constexpr size_t kMaxKeyBytes =
      static_cast<size_t>(kMaxFields) * static_cast<size_t>(kMaxFieldSize);

  // One entry of the custom tuple: a packet range, or a metadata attribute.
  struct FieldSpec {
    bess::classifier::SourceKind source =
        bess::classifier::SourceKind::kPacket;
    // The packet offset; a metadata attribute's offset is resolved per plan.
    size_t source_offset = 0;
    int attr_id = -1;  // kMetadata only
    size_t size = 0;
  };

  // The custom tuple compiled: one checked extraction plan, and what the
  // key means (its dense field bytes, and the field count for GetDesc).
  struct FieldExtraction {
    FieldExtraction(bess::classifier::ExtractPlan compiled, size_t fields)
        : plan(std::move(compiled)), num_fields(fields) {}
    bess::classifier::ExtractPlan plan;
    size_t num_fields;
  };

  struct Config {
    Mode mode = kDefaultMode;
    std::vector<gate_idx_t> gates;
    // kOther only: the fields as set (kept so a resume can rebuild the plan
    // against the metadata offsets it assigns) and the plan in use. A null
    // plan is fail-closed: no key, so every packet is dropped.
    std::vector<FieldSpec> field_specs;
    std::shared_ptr<const FieldExtraction> extraction;
    // What set it, as set (mode and fields as set_mode, gates as set_gates
    // got them): the settings resource's read-back.
    bess::pb::HashLBSettings settings;
  };

  // A metadata attribute the custom tuple names, registered once: adding the
  // same name again fails with EEXIST, and re-applying the fields (or
  // rebuilding them on a resume) must not.
  struct ResolvedAttr {
    std::string name;
    size_t size = 0;
    int attr_id = -1;
  };

  template <Mode mode>
  inline void DoProcessBatch(Context *ctx, bess::PacketBatch *batch,
                             const Config &config);

  // A copy of the current configuration (or a default one before Init).
  Config Current() const;
  // Validates and applies a mode/field change to `config`. `allow_new_attrs`
  // is false for a live settings update: a newly named metadata attribute's
  // offset is assigned by a resume, and a resource write does not resume, so
  // such an update is refused instead of hashing an unassigned offset.
  CommandResponse ApplyMode(const bess::pb::HashLBCommandSetModeArg &arg,
                            Config *config, bool allow_new_attrs);
  // Validates every wire gate before narrowing it, then replaces the list.
  CommandResponse ApplyGates(const bess::pb::HashLBCommandSetGatesArg &arg,
                             Config *config) const;
  // Parses the custom tuple in field order, resolving (once) the metadata
  // attributes it names.
  CommandResponse ParseFields(
      const google::protobuf::RepeatedPtrField<bess::pb::Field> &fields,
      std::vector<FieldSpec> *specs, bool allow_new_attrs);
  // Registers `name` (if allowed and not registered yet) and returns its id.
  CommandResponse ResolveAttr(const std::string &name, size_t size,
                              int *attr_id, bool allow_new);
  // Compiles `specs` against the metadata offsets assigned right now.
  CommandResponse BuildExtraction(
      const std::vector<FieldSpec> &specs,
      std::shared_ptr<const FieldExtraction> *extraction) const;
  // Rebuilds the custom tuple's plan on a resume: metadata offsets are
  // assigned then, after the plan was first built.
  int OnEvent(bess::Event event) override;
  void Install(Config config);

  bess::rcu::RcuPtr<Config> config_;

  // Attributes the custom tuple names (see ResolvedAttr); written under the
  // control-plane lock, like every configuration change.
  std::vector<ResolvedAttr> resolved_attrs_;

  // "<name>/settings": stages a whole Config, swaps it in; after config_.
  bess::framework::Singleton<bess::pb::HashLBSettings,
                             std::unique_ptr<const Config>>
      settings_;
};

#endif  // BESS_MODULES_HASHLB_H_
