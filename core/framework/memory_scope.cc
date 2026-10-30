// SPDX-License-Identifier: BSD-3-Clause

#include "framework/memory_scope.h"

#include <algorithm>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace bess::framework {

namespace detail {
// Admitted bytes by where they are (D-206): the DPDK part on hugepages or on
// normal pages, and the glibc part.
struct Placed {
  size_t hugepage = 0, normal = 0, glibc = 0;
  size_t total() const noexcept { return hugepage + normal + glibc; }
};
}  // namespace detail

namespace {

using detail::Placed;

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

// Each account (a scope's resource entry, a lease) by the ticket it arms
// placement with (utils::ArmPlacement, D-206), under Mu(): an allocation
// that lands on the other backing than admitted moves its bytes here.
// Tickets are never reused, so a thread still armed with a gone account's
// ticket moves nothing.
void OnPlacementMoved(uint64_t ticket, size_t bytes,
                      utils::TableBacking now_on) noexcept;

std::unordered_map<uint64_t, Placed *> &Tickets() {
  static auto *tickets = [] {
    utils::SetPlacementObserver(&OnPlacementMoved);
    return new std::unordered_map<uint64_t, Placed *>;
  }();
  return *tickets;
}

uint64_t NewTicketLocked(Placed *placed) {
  static uint64_t next = 1;
  const uint64_t ticket = next++;
  Tickets().emplace(ticket, placed);
  return ticket;
}

void OnPlacementMoved(uint64_t ticket, size_t bytes,
                      utils::TableBacking now_on) noexcept {
  std::lock_guard<std::mutex> lock(Mu());
  auto it = Tickets().find(ticket);
  if (it == Tickets().end()) {
    return;
  }
  Placed &placed = *it->second;
  size_t &from = now_on == utils::TableBacking::kNormal ? placed.hugepage
                                                        : placed.normal;
  size_t &to = now_on == utils::TableBacking::kNormal ? placed.normal
                                                      : placed.hugepage;
  const size_t moved = std::min(from, bytes);
  from -= moved;
  to += moved;
}

// Takes `bytes` back, `glibc_bytes` of them glibc's, the rest
// hugepage-counted first: when an owner releases less than it holds and
// admitted under both backings, freeing cap room it did not use is the error
// that heals (DPDK's --socket-limit still bounds the heap), keeping cap room
// it no longer uses is the one that refuses tables forever.
void Take(Placed *placed, size_t bytes, size_t glibc_bytes) {
  glibc_bytes = std::min({glibc_bytes, bytes, placed->glibc});
  placed->glibc -= glibc_bytes;
  bytes -= glibc_bytes;
  const size_t from_huge = std::min(placed->hugepage, bytes);
  placed->hugepage -= from_huge;
  utils::ReleaseTable(utils::TableBacking::kHugepages, from_huge);
  bytes -= from_huge;
  const size_t from_normal = std::min(placed->normal, bytes);
  placed->normal -= from_normal;
  placed->glibc -= std::min(placed->glibc, bytes - from_normal);
}

// Records what AdmitTable(bytes, glibc_bytes) admitted with `backing`, and
// arms this thread's placement with its DPDK part under `ticket`.
void Add(Placed *placed, uint64_t ticket, utils::TableBacking backing,
         size_t bytes, size_t glibc_bytes) {
  glibc_bytes = std::min(glibc_bytes, bytes);
  placed->glibc += glibc_bytes;
  (backing == utils::TableBacking::kHugepages ? placed->hugepage
                                              : placed->normal) +=
      bytes - glibc_bytes;
  utils::ArmPlacement(ticket, backing, bytes - glibc_bytes);
}

}  // namespace

namespace detail {

struct LeaseNode final : utils::MemoryGrowth {
  MemoryScope *scope = nullptr;  // nullptr once the scope is gone
  std::vector<LeaseNode *> *siblings = nullptr;
  Placed placed;
  uint64_t ticket = 0;  // 0 once the scope is gone
  const void *object = nullptr;
  size_t (*read)(const void *) = nullptr;

  void Grow(size_t bytes, size_t glibc_bytes) override {
    const utils::TableBacking backing = utils::AdmitTable(bytes, glibc_bytes);
    std::lock_guard<std::mutex> lock(Mu());
    if (scope == nullptr) {
      // The scope is gone and released everything; so is this growth.
      utils::ReleaseTable(backing, bytes - std::min(glibc_bytes, bytes));
      utils::ArmPlacement(0, backing, 0);
      return;
    }
    Add(&placed, ticket, backing, bytes, glibc_bytes);
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    std::lock_guard<std::mutex> lock(Mu());
    Take(&placed, bytes, glibc_bytes);
  }
};

}  // namespace detail

struct MemoryScope::Entry {
  Placed placed;  // Admit()ed, not through a lease
  uint64_t ticket = 0;
  std::vector<detail::LeaseNode *> leases;
};

size_t MemoryLease::bytes() const noexcept {
  if (node_ == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(Mu());
  return node_->placed.total();
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
      utils::ReleaseTable(utils::TableBacking::kHugepages,
                          node_->placed.hugepage);
      Tickets().erase(node_->ticket);
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
    utils::ReleaseTable(utils::TableBacking::kHugepages,
                        entry->placed.hugepage);
    Tickets().erase(entry->ticket);
    // A lease that outlives its scope (not reachable from a module: the
    // scope is a Module member, its leases the subclass's) releases nothing
    // later.
    for (detail::LeaseNode *node : entry->leases) {
      utils::ReleaseTable(utils::TableBacking::kHugepages,
                          node->placed.hugepage);
      Tickets().erase(node->ticket);
      node->scope = nullptr;
      node->siblings = nullptr;
      node->placed = {};
      node->ticket = 0;
      node->object = nullptr;
      node->read = nullptr;
    }
  }
}

int MemoryScope::socket() const { return utils::MemoryNode(); }

MemoryScope::Entry &MemoryScope::EntryLocked(std::string_view resource) {
  auto it = entries_.find(resource);
  if (it == entries_.end()) {
    auto entry = std::make_unique<Entry>();
    const uint64_t ticket = NewTicketLocked(&entry->placed);
    entry->ticket = ticket;
    try {
      it = entries_.emplace(std::string(resource), std::move(entry)).first;
    } catch (...) {
      Tickets().erase(ticket);
      throw;
    }
  }
  return *it->second;
}

void MemoryScope::Admit(std::string_view resource, size_t bytes,
                        size_t glibc_bytes) {
  const utils::TableBacking backing = utils::AdmitTable(bytes, glibc_bytes);
  try {
    std::lock_guard<std::mutex> lock(Mu());
    Entry &entry = EntryLocked(resource);
    Add(&entry.placed, entry.ticket, backing, bytes, glibc_bytes);
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
    Take(&it->second->placed, bytes, glibc_bytes);
  }
}

MemoryLease MemoryScope::Lease(std::string_view resource, size_t bytes,
                               size_t glibc_bytes) {
  const utils::TableBacking backing = utils::AdmitTable(bytes, glibc_bytes);
  try {
    auto node = std::make_unique<detail::LeaseNode>();
    std::lock_guard<std::mutex> lock(Mu());
    Entry &entry = EntryLocked(resource);
    entry.leases.reserve(entry.leases.size() + 1);
    node->ticket = NewTicketLocked(&node->placed);
    Add(&node->placed, node->ticket, backing, bytes, glibc_bytes);
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
    total += entry->placed.total();
    for (const detail::LeaseNode *node : entry->leases) {
      total += node->placed.total();
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
        Placed placed = entry->placed;
        row.current_bytes = placed.total();
        for (const detail::LeaseNode *node : entry->leases) {
          placed.hugepage += node->placed.hugepage;
          placed.normal += node->placed.normal;
          placed.glibc += node->placed.glibc;
          if (node->read != nullptr) {
            tracked.push_back({rows.size(), node->object, node->read});
          } else {
            row.current_bytes += node->placed.total();
          }
        }
        row.admitted_bytes = placed.total();
        row.hugepage_bytes = placed.hugepage;
        row.normal_bytes = placed.normal;
        row.glibc_bytes = placed.glibc;
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
