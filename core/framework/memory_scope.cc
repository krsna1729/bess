// SPDX-License-Identifier: BSD-3-Clause

#include "framework/memory_scope.h"

#include <algorithm>
#include <mutex>
#include <tuple>

#include "runtime/malloc_policy.h"

namespace bess::framework {

namespace {

// One mutex for every scope, lease and listing: admission happens on the
// control path, a handful of times per command, and GetResourceStatus lists
// under it. utils::AdmitTable runs outside it (it reads cgroupfs); its own
// count is taken under its own mutex, never while waiting for this one.
std::mutex &Mu() {
  static auto *mu = new std::mutex;  // never destroyed: modules die at exit
  return *mu;
}

std::vector<MemoryScope *> &Scopes() {
  static auto *scopes = new std::vector<MemoryScope *>;
  return *scopes;
}

// Takes `bytes` back from a hugepage-counted/normal pair, `glibc_bytes` of
// them from the normal side (glibc is never counted), the rest
// hugepage-counted first: when an owner releases less than it holds and
// admitted under both backings, freeing cap room it did not use is the error
// that heals (DPDK's --socket-limit still bounds the heap), keeping cap room
// it no longer uses is the one that refuses tables forever. What it takes is
// noted for the maintenance loop's trim (runtime/malloc_policy.h).
void Take(size_t *huge, size_t *normal, size_t bytes, size_t glibc_bytes) {
  const size_t held = *huge + *normal;
  glibc_bytes = std::min({glibc_bytes, bytes, *normal});
  *normal -= glibc_bytes;
  bytes -= glibc_bytes;
  const size_t from_huge = std::min(*huge, bytes);
  *huge -= from_huge;
  utils::ReleaseTable(utils::TableBacking::kHugepages, from_huge);
  *normal -= std::min(*normal, bytes - from_huge);
  runtime::NoteReleasedBytes(held - (*huge + *normal));
}

// Records what AdmitTable(bytes, glibc_bytes) admitted with `backing`.
void Add(size_t *huge, size_t *normal, utils::TableBacking backing,
         size_t bytes, size_t glibc_bytes) {
  glibc_bytes = std::min(glibc_bytes, bytes);
  if (backing == utils::TableBacking::kHugepages) {
    *huge += bytes - glibc_bytes;
    *normal += glibc_bytes;
  } else {
    *normal += bytes;
  }
}

}  // namespace

namespace detail {

struct LeaseNode final : utils::MemoryGrowth {
  MemoryScope *scope = nullptr;  // nullptr once the scope is gone
  std::vector<LeaseNode *> *siblings = nullptr;
  size_t huge = 0, normal = 0;
  const void *object = nullptr;
  size_t (*read)(const void *) = nullptr;

  void Grow(size_t bytes, size_t glibc_bytes) override {
    const utils::TableBacking backing = utils::AdmitTable(bytes, glibc_bytes);
    std::lock_guard<std::mutex> lock(Mu());
    if (scope == nullptr) {
      // The scope is gone and released everything; so is this growth.
      utils::ReleaseTable(backing, bytes - std::min(glibc_bytes, bytes));
      return;
    }
    Add(&huge, &normal, backing, bytes, glibc_bytes);
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    std::lock_guard<std::mutex> lock(Mu());
    Take(&huge, &normal, bytes, glibc_bytes);
  }
};

}  // namespace detail

struct MemoryScope::Entry {
  size_t huge = 0, normal = 0;  // Admit()ed, not through a lease
  std::vector<detail::LeaseNode *> leases;
};

size_t MemoryLease::bytes() const noexcept {
  if (node_ == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(Mu());
  return node_->huge + node_->normal;
}

utils::MemoryGrowth *MemoryLease::growth() const noexcept { return node_; }

void MemoryLease::Track(const void *object, size_t (*read)(const void *)) noexcept {
  if (node_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(Mu());
  node_->object = object;
  node_->read = read;
}

void MemoryLease::reset() noexcept {
  if (node_ == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(Mu());
    if (node_->scope != nullptr) {
      auto &siblings = *node_->siblings;
      siblings.erase(std::find(siblings.begin(), siblings.end(), node_));
      utils::ReleaseTable(utils::TableBacking::kHugepages, node_->huge);
      runtime::NoteReleasedBytes(node_->huge + node_->normal);
    }
  }
  delete node_;
  node_ = nullptr;
}

MemoryScope::MemoryScope(const std::string *owner) : owner_(owner) {
  std::lock_guard<std::mutex> lock(Mu());
  Scopes().push_back(this);
}

MemoryScope::~MemoryScope() {
  std::lock_guard<std::mutex> lock(Mu());
  auto &scopes = Scopes();
  scopes.erase(std::find(scopes.begin(), scopes.end(), this));
  for (auto &[name, entry] : entries_) {
    utils::ReleaseTable(utils::TableBacking::kHugepages, entry->huge);
    runtime::NoteReleasedBytes(entry->huge + entry->normal);
    // A lease that outlives its scope (not reachable from a module: the
    // scope is a Module member, its leases the subclass's) releases nothing
    // later.
    for (detail::LeaseNode *node : entry->leases) {
      utils::ReleaseTable(utils::TableBacking::kHugepages, node->huge);
      runtime::NoteReleasedBytes(node->huge + node->normal);
      node->scope = nullptr;
      node->siblings = nullptr;
      node->huge = node->normal = 0;
      node->object = nullptr;
      node->read = nullptr;
    }
  }
}

int MemoryScope::socket() const { return utils::MemoryNode(); }

MemoryScope::Entry &MemoryScope::EntryLocked(std::string_view resource) {
  auto it = entries_.find(resource);
  if (it == entries_.end()) {
    it = entries_.emplace(std::string(resource), std::make_unique<Entry>()).first;
  }
  return *it->second;
}

void MemoryScope::Admit(std::string_view resource, size_t bytes,
                        size_t glibc_bytes) {
  const utils::TableBacking backing = utils::AdmitTable(bytes, glibc_bytes);
  try {
    std::lock_guard<std::mutex> lock(Mu());
    Entry &entry = EntryLocked(resource);
    Add(&entry.huge, &entry.normal, backing, bytes, glibc_bytes);
  } catch (...) {
    utils::ReleaseTable(backing, bytes - std::min(glibc_bytes, bytes));
    throw;
  }
}

void MemoryScope::Release(std::string_view resource, size_t bytes,
                          size_t glibc_bytes) noexcept {
  std::lock_guard<std::mutex> lock(Mu());
  auto it = entries_.find(resource);
  if (it != entries_.end()) {
    Take(&it->second->huge, &it->second->normal, bytes, glibc_bytes);
  }
}

MemoryLease MemoryScope::Lease(std::string_view resource, size_t bytes,
                               size_t glibc_bytes) {
  const utils::TableBacking backing = utils::AdmitTable(bytes, glibc_bytes);
  try {
    auto node = std::make_unique<detail::LeaseNode>();
    Add(&node->huge, &node->normal, backing, bytes, glibc_bytes);
    std::lock_guard<std::mutex> lock(Mu());
    Entry &entry = EntryLocked(resource);
    entry.leases.push_back(node.get());
    node->scope = this;
    node->siblings = &entry.leases;
    return MemoryLease(node.release());
  } catch (...) {
    utils::ReleaseTable(backing, bytes - std::min(glibc_bytes, bytes));
    throw;
  }
}

size_t MemoryScope::admitted_bytes() const {
  std::lock_guard<std::mutex> lock(Mu());
  size_t total = 0;
  for (const auto &[name, entry] : entries_) {
    total += entry->huge + entry->normal;
    for (const detail::LeaseNode *node : entry->leases) {
      total += node->huge + node->normal;
    }
  }
  return total;
}

MemoryScope &MemoryAccounts::Process() {
  static const auto *owner = new std::string("bessd");
  static auto *scope = new MemoryScope(owner);  // never destroyed
  return *scope;
}

std::vector<ModuleMemoryUse> MemoryAccounts::List() {
  // The tracked objects' bytes() are read after the mutex is released: a
  // primitive takes its own lock in bytes() (ObjectSlab) and holds it while
  // its growth hook takes this mutex, so reading them under it would order
  // the two locks both ways. The objects stay alive meanwhile because the
  // caller holds the control-plane lock their owners create, grow and
  // destroy them under.
  struct Tracked {
    size_t row;
    const void *object;
    size_t (*read)(const void *);
  };
  std::vector<ModuleMemoryUse> rows;
  std::vector<Tracked> tracked;
  {
    std::lock_guard<std::mutex> lock(Mu());
    for (const MemoryScope *scope : Scopes()) {
      for (const auto &[name, entry] : scope->entries_) {
        ModuleMemoryUse row;
        row.admitted_bytes = row.current_bytes = entry->huge + entry->normal;
        for (const detail::LeaseNode *node : entry->leases) {
          const size_t admitted = node->huge + node->normal;
          row.admitted_bytes += admitted;
          if (node->read != nullptr) {
            tracked.push_back({rows.size(), node->object, node->read});
          } else {
            row.current_bytes += admitted;
          }
        }
        row.module = *scope->owner_;
        row.resource = name;
        rows.push_back(std::move(row));
      }
    }
  }
  for (const Tracked &t : tracked) {
    rows[t.row].current_bytes += t.read(t.object);
  }
  std::erase_if(rows, [](const ModuleMemoryUse &row) {
    return row.admitted_bytes == 0 && row.current_bytes == 0;
  });
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    return std::tie(a.module, a.resource) < std::tie(b.module, b.resource);
  });
  return rows;
}

}  // namespace bess::framework
