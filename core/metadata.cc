// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "metadata.h"

#include "utils/logging.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <utility>

#include "module.h"
#include "module_graph.h"

namespace bess {
namespace metadata {

// TODO: Once the rest of the code supports multiple pipelines, this ought to be
// a collection of pipelines in bess::metadata a la Ports/Modules.
Pipeline default_pipeline;

// Helpers -----------------------------------------------------------------

static mt_offset_t ComputeNextOffset(mt_offset_t curr_offset, int8_t size) {
  uint32_t overflow;
  int8_t rounded_size;

  rounded_size = align_ceil_pow2(size);

  if (curr_offset % rounded_size) {
    curr_offset = align_ceil(curr_offset, rounded_size);
  }

  overflow = (uint32_t)curr_offset + (uint32_t)size;

  return overflow > kMetadataTotalSize ? kMetadataOffsetNoSpace : curr_offset;
}

// Generate warnings for modules that read metadata that never gets set, and
// fill the report the control plane exposes. The offset assignment has already
// run: this function only classifies declarations, applies default contracts
// to readers with no provider, and logs.
void Pipeline::ApplyContractsAndBuildReport() {
  using AccessMode = Attribute::AccessMode;
  using Contract = Attribute::Contract;

  report_ = MetadataReport{};

  // A defaulted reader the graph did not resolve reads its declared value at
  // no packet-path cost. This is the safe point: workers are paused, offsets
  // are final for this resume, and no packet memory is touched.
  for (const auto &it : ModuleGraph::GetAllModules()) {
    Module *m = it.second.get();
    if (!m) {
      break;
    }
    size_t i = 0;
    for (const auto &attr : m->all_attrs()) {
      if ((attr.mode == AccessMode::kRead ||
           attr.mode == AccessMode::kUpdate) &&
          m->attr_offset(i) == kMetadataOffsetNoRead &&
          attr.contract == Contract::kDefault) {
        m->set_attr_offset(i, kMetadataOffsetDefault);
      }
      i++;
    }
  }

  std::map<std::string, std::vector<const Module *>> declarations;
  // Writers and readers by attribute name: more than one writer of a name a
  // reader consumes is the duplicate/conflicting-declaration case.
  std::map<std::string, int> writer_counts;
  std::map<std::string, bool> has_reader;
  // A reader in more than one scope component is reachable from more than one
  // writer scope: the allocator gives those scopes separate offsets and the
  // reader ends up with one of them, so the graph is ambiguous. Count the
  // components that claim each (module, attribute) pair.
  std::map<std::pair<std::string, std::string>, int> component_claims;
  for (const ScopeComponent &component : scope_components_) {
    for (const Module *m : component.modules()) {
      for (const auto &attr : m->all_attrs()) {
        if (attr.name == component.attr_id() &&
            (attr.mode == AccessMode::kRead ||
             attr.mode == AccessMode::kUpdate)) {
          component_claims[{m->name(), attr.name}]++;
          break;
        }
      }
    }
  }
  std::map<std::string, std::vector<std::string>> multiple_provider_readers;
  for (const auto &[key, count] : component_claims) {
    if (count > 1) {
      multiple_provider_readers[key.second].push_back(key.first);
    }
  }

  for (const auto &it : ModuleGraph::GetAllModules()) {
    Module *m = it.second.get();
    if (!m) {
      break;
    }
    size_t i = 0;
    for (const auto &attr : m->all_attrs()) {
      MetadataDeclaration declaration;
      declaration.module = m->name();
      declaration.attribute = attr.name;
      declaration.size = attr.size;
      declaration.mode = attr.mode;
      declaration.contract = attr.contract;
      declaration.default_value.assign(attr.default_value.begin(),
                                       attr.default_value.begin() + attr.size);
      declarations[attr.name].push_back(m);

      const mt_offset_t offset = m->attr_offset(i);
      report_.declarations++;
      if (IsValidOffset(offset)) {
        report_.resolved++;
      }
      if (attr.mode == AccessMode::kWrite) {
        writer_counts[attr.name]++;
      } else {
        has_reader[attr.name] = true;
      }
      if (attr.mode == AccessMode::kWrite) {
        if (offset == kMetadataOffsetNoWrite) {
          report_.writers_without_reader.push_back(declaration);
          VLOG(1) << "Metadata attr " << attr.name << "/" << attr.size
                  << " of module " << m->name()
                  << " has no downstream reader; the write is skipped";
        }
      } else if (offset == kMetadataOffsetNoRead) {
        report_.readers_without_writer.push_back(declaration);
        if (attr.contract == Contract::kRequired) {
          report_.unsatisfied_required.push_back(declaration);
          LOG(ERROR) << "Required metadata attr " << attr.name << "/"
                     << attr.size << " of module " << m->name()
                     << " has no upstream module that writes it";
        } else {
          LOG(WARNING) << "Metadata attr " << attr.name << "/" << attr.size
                       << " of module " << m->name()
                       << " has no upstream module that sets the value!";
        }
      } else if (offset == kMetadataOffsetDefault) {
        report_.defaults_applied.push_back(declaration);
        VLOG(1) << "Metadata attr " << attr.name << "/" << attr.size
                << " of module " << m->name()
                << " has no writer; its declared default applies";
      }
      i++;
    }
  }

  // Declarations of one name must agree on what a missing writer means. The
  // registry already refuses a size disagreement when the second module is
  // created, so only contract/default disagreements can reach here.
  for (const auto &entry : declarations) {
    const std::string &name = entry.first;
    const Attribute *first_reader = nullptr;
    std::string reason;
    for (const Module *m : entry.second) {
      const Attribute *attr = nullptr;
      for (const auto &candidate : m->all_attrs()) {
        if (candidate.name == name) {
          attr = &candidate;
          break;
        }
      }
      if (attr == nullptr || (attr->mode != AccessMode::kRead &&
                              attr->mode != AccessMode::kUpdate)) {
        continue;
      }
      if (first_reader == nullptr) {
        first_reader = attr;
        continue;
      }
      if (first_reader->contract != attr->contract) {
        reason = "declarations disagree: a reader declares a different contract";
        break;
      }
      if (attr->contract == Contract::kDefault &&
          std::memcmp(first_reader->default_value.data(),
                      attr->default_value.data(),
                      std::min(first_reader->size, attr->size)) != 0) {
        reason = "declarations disagree: default values differ";
        break;
      }
    }
    if (reason.empty() &&
        (writer_counts[name] > 1 && has_reader[name])) {
      reason =
          "more than one writer scope declares the attribute for a reader; "
          "each scope has its own offset";
    }
    if (reason.empty()) {
      const auto providers = multiple_provider_readers.find(name);
      if (providers != multiple_provider_readers.end()) {
        reason =
            "readers are reachable from more than one writer scope; only one "
            "scope's offset is used";
      }
    }
    if (!reason.empty()) {
      MetadataConflict conflict;
      conflict.attribute = name;
      conflict.reason = reason;
      for (const Module *m : entry.second) {
        conflict.modules.push_back(m->name());
      }
      report_.conflicts.push_back(std::move(conflict));
      LOG(WARNING) << "Metadata attr " << name << ": " << reason;
    }
  }
}

static inline attr_id_t get_attr_id(const struct Attribute *attr) {
  return attr->name;
}

// ScopeComponent ----------------------------------------------------------

class ScopeComponentComp {
 public:
  explicit ScopeComponentComp(const bool &revparam = false)
      : reverse_(revparam) {}

  bool operator()(const ScopeComponent *lhs, const ScopeComponent *rhs) const {
    if (reverse_) {
      return (lhs->offset() < rhs->offset());
    }
    return (lhs->offset() > rhs->offset());
  }

 private:
  bool reverse_;
};

static bool DegreeComp(const ScopeComponent &a, const ScopeComponent &b) {
  return a.degree() > b.degree();
}

bool ScopeComponent::DisjointFrom(const ScopeComponent &rhs) {
  for (const auto &i : modules_) {
    for (const auto &j : rhs.modules_) {
      if (i == j) {
        return 0;
      }
    }
  }
  return 1;
}

// Pipeline ----------------------------------------------------------------

int Pipeline::PrepareMetadataComputation() {
  for (const auto &it : ModuleGraph::GetAllModules()) {
    Module *m = it.second.get();
    if (!m) {
      break;
    }

    if (!module_components_.count(m)) {
      module_components_.emplace(m, new scope_id_t[kMetadataTotalSize]);
    }

    if (module_components_[m] == nullptr) {
      return -ENOMEM;
    }

    module_scopes_[m] = -1;
    memset(module_components_[m], -1, sizeof(scope_id_t) * kMetadataTotalSize);

    for (const auto &attr : m->all_attrs()) {
      attr.scope_id = -1;
    }
  }
  return 0;
}

void Pipeline::CleanupMetadataComputation() {
  for (const auto &it : module_components_) {
    delete[] module_components_[it.first];
  }
  module_components_.clear();
  module_scopes_.clear();

  for (auto &c : scope_components_) {
    c.clear_modules();
  }
  scope_components_.clear();
}

void Pipeline::AddModuleToComponent(Module *m, const struct Attribute *attr) {
  ScopeComponent &component = scope_components_.back();

  // Module has already been added to current scope component.
  if (std::find(component.modules().begin(), component.modules().end(), m) !=
      component.modules().end()) {
    return;
  }

  if (component.modules().empty()) {
    component.set_attr_id(get_attr_id(attr));
    component.set_size(attr->size);
  }
  component.add_module(m);
}

const struct Attribute *Pipeline::FindAttr(Module *m,
                                           const struct Attribute *attr) const {
  for (const auto &it : m->all_attrs()) {
    if (get_attr_id(&it) == get_attr_id(attr)) {
      return &it;
    }
  }

  return nullptr;
}

void Pipeline::TraverseUpstream(Module *m, const struct Attribute *attr) {
  const struct Attribute *found_attr;

  AddModuleToComponent(m, attr);
  found_attr = FindAttr(m, attr);

  /* end of scope component */
  if (found_attr && found_attr->mode == Attribute::AccessMode::kWrite) {
    if (found_attr->scope_id == -1)
      IdentifyScopeComponent(m, found_attr);
    return;
  }

  /* cycle detection */
  if (module_scopes_[m] == static_cast<int>(scope_components_.size())) {
    return;
  }
  module_scopes_[m] = static_cast<int>(scope_components_.size());

  for (const IGate *g : m->igates()) {
    if (g == nullptr) {
      continue;
    }
    for (const auto &og : g->ogates_upstream()) {
      TraverseUpstream(og->module(), attr);
    }
  }

  if (m->igates().size() == 0) {
    scope_components_.back().set_invalid(true);
  }
}

int Pipeline::TraverseDownstream(Module *m, const struct Attribute *attr) {
  const struct Attribute *found_attr;
  int8_t in_scope = 0;

  // cycle detection
  if (module_scopes_[m] == static_cast<int>(scope_components_.size())) {
    return -1;
  }
  module_scopes_[m] = static_cast<int>(scope_components_.size());

  found_attr = FindAttr(m, attr);

  if (found_attr && (found_attr->mode == Attribute::AccessMode::kRead ||
                     found_attr->mode == Attribute::AccessMode::kUpdate)) {
    AddModuleToComponent(m, found_attr);
    found_attr->scope_id = scope_components_.size();

    for (const auto &ogate : m->ogates()) {
      if (!ogate) {
        continue;
      }

      TraverseDownstream(ogate->igate()->module(), attr);
    }

    module_scopes_[m] = -1;
    TraverseUpstream(m, attr);
    in_scope = 1;
    return 0;
  } else if (found_attr) {
    module_scopes_[m] = -1;
    in_scope = 0;
    return -1;
  }

  for (const auto &ogate : m->ogates()) {
    if (!ogate) {
      continue;
    }

    if (TraverseDownstream(ogate->igate()->module(), attr) != -1) {
      in_scope = 1;
    }
  }

  if (in_scope) {
    AddModuleToComponent(m, attr);
    module_scopes_[m] = -1;
    TraverseUpstream(m, attr);
  }

  return in_scope ? 0 : -1;
}

void Pipeline::IdentifySingleScopeComponent(Module *m,
                                            const struct Attribute *attr) {
  scope_components_.emplace_back();
  IdentifyScopeComponent(m, attr);
  scope_components_.back().set_scope_id(scope_components_.size());
}

void Pipeline::IdentifyScopeComponent(Module *m, const struct Attribute *attr) {
  AddModuleToComponent(m, attr);
  attr->scope_id = scope_components_.size();

  /* cycle detection */
  module_scopes_[m] = static_cast<int>(scope_components_.size());

  for (const auto &ogate : m->ogates()) {
    if (!ogate) {
      continue;
    }

    TraverseDownstream(ogate->igate()->module(), attr);
  }
}

void Pipeline::FillOffsetArrays() {
  for (size_t i = 0; i < scope_components_.size(); i++) {
    const std::set<Module *> &modules = scope_components_[i].modules();
    const attr_id_t &id = scope_components_[i].attr_id();
    int size = scope_components_[i].size();
    mt_offset_t offset = scope_components_[i].offset();
    uint8_t invalid = scope_components_[i].invalid();

    // attr not read donwstream.
    if (modules.size() == 1) {
      scope_components_[i].set_offset(kMetadataOffsetNoWrite);
      offset = kMetadataOffsetNoWrite;
    }

    for (Module *m : modules) {
      size_t k = 0;
      for (const auto &attr : m->all_attrs()) {
        if (get_attr_id(&attr) == id) {
          if (invalid) {
            // An update reads before it writes, so an upstream gap is a
            // missing read. A writer is skipped (its offset is negative), but
            // its value has a reader: kMetadataOffsetInvalidScope, not the
            // "no reader" kMetadataOffsetNoWrite.
            if (attr.mode == Attribute::AccessMode::kRead ||
                attr.mode == Attribute::AccessMode::kUpdate) {
              m->set_attr_offset(k, kMetadataOffsetNoRead);
            } else {
              m->set_attr_offset(k, kMetadataOffsetInvalidScope);
            }
          } else {
            m->set_attr_offset(k, offset);
          }
          break;
        }
        k++;
      }

      if (!invalid && offset >= 0) {
        for (int l = 0; l < size; l++) {
          module_components_[m][offset + l] = i;
        }
      }
    }
  }
}

void Pipeline::AssignOffsets() {
  mt_offset_t offset = 0;
  ScopeComponent *comp1;
  const ScopeComponent *comp2;

  for (size_t i = 0; i < scope_components_.size(); i++) {
    std::priority_queue<const ScopeComponent *,
                        std::vector<const ScopeComponent *>, ScopeComponentComp>
        h;
    comp1 = &scope_components_[i];

    if (comp1->invalid()) {
      comp1->set_offset(kMetadataOffsetNoRead);
      comp1->set_assigned(true);
      continue;
    }

    if (comp1->assigned() || comp1->modules().size() == 1) {
      continue;
    }

    offset = 0;

    for (size_t j = 0; j < scope_components_.size(); j++) {
      if (i == j) {
        continue;
      }

      if (!scope_components_[i].DisjointFrom(scope_components_[j]) &&
          scope_components_[j].assigned()) {
        h.push(&scope_components_[j]);
      }
    }

    while (!h.empty()) {
      comp2 = h.top();
      h.pop();

      if (comp2->offset() == kMetadataOffsetNoRead ||
          comp2->offset() == kMetadataOffsetNoWrite ||
          comp2->offset() == kMetadataOffsetNoSpace) {
        continue;
      }

      if (offset + comp1->size() > comp2->offset()) {
        offset =
            ComputeNextOffset(comp2->offset() + comp2->size(), comp1->size());
      } else {
        break;
      }
    }

    comp1->set_offset(offset);
    comp1->set_assigned(true);
  }

  FillOffsetArrays();
}

void Pipeline::LogAllScopes() const {
  for (size_t i = 0; i < scope_components_.size(); i++) {
    VLOG(1) << "scope component for " << scope_components_[i].size()
            << "-byte attr " << scope_components_[i].attr_id() << " at offset "
            << static_cast<int>(scope_components_[i].offset()) << ": {";

    for (const auto &it : scope_components_[i].modules()) {
      VLOG(1) << it->name();
    }

    VLOG(1) << "}";
  }

  for (const auto &it : ModuleGraph::GetAllModules()) {
    const Module *m = it.second.get();
    if (!m) {
      break;
    }

    const scope_id_t *scope_arr = module_components_.find(m)->second;

    LOG(INFO) << "Module " << m->name()
              << " part of the following scope components: ";
    for (size_t i = 0; i < kMetadataTotalSize; i++) {
      if (scope_arr[i] != -1) {
        LOG(INFO) << "scope " << scope_arr[i] << " at offset " << i;
      }
    }
  }
}

void Pipeline::ComputeScopeDegrees() {
  for (size_t i = 0; i < scope_components_.size(); i++) {
    for (size_t j = i + 1; j < scope_components_.size(); j++) {
      if (!scope_components_[i].DisjointFrom(scope_components_[j])) {
        scope_components_[i].incr_degree();
        scope_components_[j].incr_degree();
      }
    }
  }
}

/* Main entry point for calculating metadata offsets. */
int Pipeline::ComputeMetadataOffsets() {
  int ret;

  ret = PrepareMetadataComputation();

  if (ret) {
    CleanupMetadataComputation();
    return ret;
  }

  for (const auto &it : ModuleGraph::GetAllModules()) {
    Module *m = it.second.get();
    if (!m) {
      break;
    }

    size_t i = 0;
    for (const auto &attr : m->all_attrs()) {
      if (attr.mode == Attribute::AccessMode::kRead ||
          attr.mode == Attribute::AccessMode::kUpdate) {
        m->set_attr_offset(i, kMetadataOffsetNoRead);
      } else if (attr.mode == Attribute::AccessMode::kWrite) {
        m->set_attr_offset(i, kMetadataOffsetNoWrite);
        if (attr.scope_id == -1) {
          IdentifySingleScopeComponent(m, &attr);
        }
      }
      i++;
    }
  }

  ComputeScopeDegrees();
  std::sort(scope_components_.begin(), scope_components_.end(), DegreeComp);
  AssignOffsets();

  if (VLOG_IS_ON(1)) {
    LogAllScopes();
  }

  ApplyContractsAndBuildReport();

  ret = first_unsatisfied_required() != nullptr ? -EINVAL : 0;

  CleanupMetadataComputation();
  return ret;
}

int Pipeline::RegisterAttribute(const std::string &attr_name, size_t size) {
  const auto &it = registered_attrs_.find(attr_name);
  if (it == registered_attrs_.end()) {
    registered_attrs_.emplace(attr_name, std::make_tuple(size, 1));
    return 0;
  }

  size_t registered_size = std::get<0>(it->second);
  int &count = std::get<1>(it->second);

  if (registered_size == size) {
    count++;
    return 0;
  } else {
    LOG(ERROR) << "Attribute '" << attr_name
               << "' has size mismatch: registered(" << registered_size
               << ") vs new(" << size << ")";
    return -EINVAL;
  }
}

void Pipeline::DeregisterAttribute(const std::string &attr_name) {
  const auto &it = registered_attrs_.find(attr_name);
  if (it == registered_attrs_.end()) {
    LOG(ERROR) << "ReregisteredAttribute() called, but '" << attr_name
               << "' was not registered";
    return;
  }

  int &count = std::get<1>(it->second);

  count--;
  DCHECK_GE(count, 0);

  if (count == 0) {
    // No more modules are using the attribute. Remove it from the map.
    registered_attrs_.erase(it);
  }
}

}  // namespace metadata
}  // namespace bess
