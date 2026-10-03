// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_DECISION_CACHE_H_
#define BESS_FLOW_DECISION_CACHE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#include "flow/flow_types.h"
#include "flow/worker_flow_table.h"
#include "utils/common.h"

namespace bess::flow {

// Decision cache (roadmap M12, Decision D-062).
//
// Rich policy (an OVS megaflow, a VFP layer stack, an ACL plus NAT plus
// metering) is compiled once per flow by the application into an immutable
// decision object; packets of that flow then find the decision with one flow
// lookup and one generation compare:
//
//   FlowKey --DecisionCache--> DecisionId --application table--> decision
//
// The cache knows nothing of what a decision means. `DecisionId` is the
// application's id type (any trivially copyable value, typically a StrongId it
// resolves through its own SlotTable); BESS does not require ActionId.
//
// Invalidation is by generation, O(1), and never walks a cache: every entry
// carries the generation of the policy it was compiled against, and a
// `DecisionGeneration` shared by all the caches of one policy scope holds the
// current one. `Invalidate()` is one increment; afterwards every older entry
// reads as stale (a miss) until the application recompiles and reinstalls it.
// The generation is 64 bits so it never wraps.
//
// Miss path, owned by the application (no graph module is involved):
//   1. `const uint64_t g = generation.Current();`
//   2. compile the decision for the key under the policy of generation `g`
//      (or punt the key to a control thread through M11 and wait for it);
//   3. `cache.Install(key, id, g)`. A policy change between 1 and 3 makes the
//      install refused (kStaleGeneration), so a decision compiled against old
//      policy is never installed as current -- provided the control side keeps
//      this order:
//
// Control side: publish the new policy (and the decision objects it needs)
// first, then Invalidate(). A worker that reads the new generation then
// compiles against the new policy. Invalidating first lets a worker read the
// new generation, compile against the still-old policy, and install that
// decision as current until the next Invalidate().
//
// Ownership and threads: a DecisionCache is a WorkerFlowTable, owned by one
// worker; only that worker looks up, installs and erases. The generation is
// shared: any thread may Invalidate(). A decision object must stay readable
// until no worker can still hold its id -- publish decisions through RCU
// (SlotTable) and retire them after a grace period, as for any shared object.
//
// Stale entries keep their slot until their key is installed again (reused in
// place) or erased. Keys that never return leave through Erase(), typically
// driven by M10 expiry; there is no O(capacity) sweep.

// The current generation of one policy scope (a whole application, a tenant,
// a policy group). Shared by every worker's cache for that scope.
class alignas(64) DecisionGeneration {
 public:
  // Acquire: a worker that sees generation g also sees every decision object
  // the control side published before making g current.
  uint64_t Current() const noexcept {
    return value_.load(std::memory_order_acquire);
  }

  // Makes every entry compiled before now stale and returns the new
  // generation. Release: decision objects published before the call are
  // visible to a worker that reads the new generation. Any thread.
  uint64_t Invalidate() noexcept {
    return value_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

 private:
  std::atomic<uint64_t> value_{1};
};

enum class DecisionLookup : uint8_t {
  kHit,    // a current decision
  kMiss,   // no entry for the key
  kStale,  // an entry compiled against an older generation
};

enum class DecisionInstall : uint8_t {
  kInstalled,        // a new entry
  kReplaced,         // the key's entry now holds this decision
  kStaleGeneration,  // compiled against a generation that is no longer current;
                     // nothing changed
  kFull,             // no free slot; nothing changed
};

template <typename DecisionId>
struct DecisionEntry {
  DecisionId id;
  uint64_t generation;
};

template <FixedFlowKey Key, typename DecisionId,
          typename Hash = DefaultFlowHash<Key>,
          typename Equal = DefaultFlowEqual<Key>,
          typename Traits = DefaultFlowTableTraits>
class DecisionCache {
  static_assert(std::is_trivially_copyable_v<DecisionId>,
                "a DecisionId is a value the cache copies: make it a plain id");

 public:
  using Entry = DecisionEntry<DecisionId>;
  using Table = WorkerFlowTable<Key, Entry, Hash, Equal, Traits>;
  static constexpr size_t kMaxBatch = Table::kMaxBatch;

  // An empty cache of exactly `capacity` entries for the policy scope
  // `generation`, which must outlive it. Allocations happen only here.
  static std::expected<std::unique_ptr<DecisionCache>, FlowTableError> Create(
      size_t capacity, const DecisionGeneration &generation) {
    auto table = Table::Create(capacity);
    if (!table) {
      return std::unexpected(table.error());
    }
    std::unique_ptr<DecisionCache> cache(
        new (std::nothrow) DecisionCache(std::move(*table), generation));
    if (cache == nullptr) {
      return std::unexpected(FlowTableError::kOutOfMemory);
    }
    return cache;
  }

  // The decision for `key`, or why there is none. One flow lookup and one
  // compare against the generation loaded here.
  DecisionLookup Lookup(const Key &key, DecisionId *out) const noexcept {
    const Entry *entry = table_->Find(key);
    if (entry == nullptr) {
      return DecisionLookup::kMiss;
    }
    if (entry->generation != generation_.Current()) {
      return DecisionLookup::kStale;
    }
    *out = entry->id;
    return DecisionLookup::kHit;
  }

  // Looks up `keys.size()` (<= kMaxBatch) keys, reading the generation once.
  // Bit i of the result is set when out[i] holds a current decision; bit i of
  // `*stale` (if given) is set when keys[i] had an entry that is stale. Other
  // out[] positions are untouched.
  uint64_t LookupBatch(std::span<const Key> keys, std::span<DecisionId> out,
                       uint64_t *stale = nullptr) const noexcept {
    promise(keys.size() <= kMaxBatch && out.size() >= keys.size());
    const Entry *entries[kMaxBatch];
    const uint64_t found = table_->FindBatch(
        keys, std::span<const Entry *>(entries, keys.size()));
    const uint64_t current = generation_.Current();
    uint64_t hits = 0;
    for (uint64_t m = found; m != 0; m &= m - 1) {
      const unsigned i = static_cast<unsigned>(__builtin_ctzll(m));
      if (entries[i]->generation == current) {
        out[i] = entries[i]->id;
        hits |= uint64_t{1} << i;
      }
    }
    if (stale != nullptr) {
      *stale = found & ~hits;
    }
    return hits;
  }

  // Installs `id` for `key`, compiled against generation `compiled_at` (read
  // with DecisionGeneration::Current() before compiling). Refused when the
  // policy has changed since; an existing entry for the key (stale or not) is
  // overwritten in place.
  DecisionInstall Install(const Key &key, DecisionId id,
                          uint64_t compiled_at) noexcept {
    if (compiled_at != generation_.Current()) {
      return DecisionInstall::kStaleGeneration;
    }
    auto result = table_->Emplace(key, Entry{id, compiled_at});
    switch (result.status) {
      case EmplaceStatus::kCreated:
        return DecisionInstall::kInstalled;
      case EmplaceStatus::kExists:
        *result.state = Entry{id, compiled_at};
        return DecisionInstall::kReplaced;
      default:
        return DecisionInstall::kFull;
    }
  }

  // Removes the key's entry. False if absent.
  bool Erase(const Key &key) noexcept { return table_->Erase(key); }

  uint64_t generation() const noexcept { return generation_.Current(); }
  size_t size() const noexcept { return table_->size(); }
  size_t capacity() const noexcept { return table_->capacity(); }
  size_t memory_bytes() const noexcept {
    return table_->memory_bytes() + sizeof(*this);
  }

 private:
  DecisionCache(std::unique_ptr<Table> table,
                const DecisionGeneration &generation)
      : table_(std::move(table)), generation_(generation) {}

  std::unique_ptr<Table> table_;
  const DecisionGeneration &generation_;
};

}  // namespace bess::flow

#endif  // BESS_FLOW_DECISION_CACHE_H_
