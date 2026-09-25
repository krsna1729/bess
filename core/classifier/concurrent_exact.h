// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CLASSIFIER_CONCURRENT_EXACT_H_
#define BESS_CLASSIFIER_CONCURRENT_EXACT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <rte_hash.h>

#include "classifier/backend.h"
#include "rcu/rcu_domain.h"
#include "utils/common.h"

namespace bess::classifier {

// A shared exact-match table updated in place while workers read it (G1.2
// mode C). DPDK's `rte_hash` owns the algorithm and the concurrency: created
// with RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF, lookups are lock-free and run
// while one writer adds and deletes; a deleted key's slot is handed back only
// after the runtime RcuDomain's QSBR grace period (rte_hash_rcu_qsbr_add,
// defer-queue mode), the same wiring K7 uses for rte_lpm.
//
// Why not a generation swap: that rebuilds the whole table per insert
// (ExactMatch: 47 ms per add at 100K rules). Here an insert or delete is one
// rte_hash operation, flat in table size. Lookup cost, the rte_hash modes
// chosen, and the DPDK behaviours this relies on (each pinned by a test) are
// in docs/dataplane-tables.md; Decisions D-001, D-002 (docs/decisions.md).
//
// Values are up to eight bytes, stored in rte_hash's data pointer. Writers
// must be serialized by the caller (a module's command path is).
//
// Sizing (Decision D-010). Capacity is key slots, fixed at creation; the
// owner grows by building a larger table. Owners create tables with
// CapacityFor(rules): three quarters of a power of two, so rte_hash's bucket
// array (rounded up to the next power of two) has 4/3 as many positions as
// slots and every slot is reachable (occupancy_bench: 100% for every key
// shape, and the last 10% of a fill still costs ~200 ns per add). Owners grow
// when HasRoomForOne() is false: live keys plus deletes still waiting out a
// grace period must leave Headroom() slots free, because a delete's slot is
// reusable only after every worker's next quiescent point. `Upsert` reporting
// kFull remains the backstop. Capacity is
// larger table (a deliberate, rare, amortized rebuild).
class ConcurrentExactTable {
 public:
  enum class UpsertResult : uint8_t { kInserted, kUpdated, kFull };

  // Slots of reserve for deletes pending a grace period, at least: one
  // writer's measured peak (~4M ops/s with readers present, update_scale_bench
  // E2) times a 64 us grace period. Tables also keep 5% of their slots.
  static constexpr uint32_t kMinHeadroom = 256;

  static uint32_t Headroom(uint32_t capacity) noexcept {
    return std::max(capacity / 20, kMinHeadroom);
  }

  // The smallest capacity (3/4 of a power of two, at least 768) that holds
  // `rules` keys plus its headroom.
  static uint32_t CapacityFor(size_t rules) noexcept;

  // `capacity` key slots exactly; pass CapacityFor(rules) for the D-010
  // sizing.
  static std::expected<std::unique_ptr<ConcurrentExactTable>, std::string>
  Create(uint32_t key_len, uint32_t capacity, rcu::RcuDomain &domain,
         int socket = SOCKET_ID_ANY);

  ~ConcurrentExactTable();

  ConcurrentExactTable(const ConcurrentExactTable &) = delete;
  ConcurrentExactTable &operator=(const ConcurrentExactTable &) = delete;

  // -- writer ------------------------------------------------------------------

  UpsertResult Upsert(ConstBytes key, uint64_t value);
  // False if the key is absent.
  bool Erase(ConstBytes key);

  // Whether one more key fits with Headroom() slots to spare, counting
  // deletes still waiting out a grace period (reclaims what is ready first).
  bool HasRoomForOne();

  // Reclaims every deleted slot whose grace period has passed.
  void ReclaimAll();

  // Returns to the free list the deleted slots whose grace period has
  // passed (up to DPDK's per-call batch). Adds and deletes do this on their
  // own; exposed for owners and tests.
  void Reclaim();

  // Visits every entry: fn(ConstBytes key, uint64_t value). Writer thread.
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    const void *key;
    void *data;
    uint32_t next = 0;
    while (rte_hash_iterate(table_, &key, &data, &next) >= 0) {
      fn(ConstBytes(static_cast<const std::byte *>(key), key_len_),
         static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)));
    }
  }

  size_t size() const noexcept { return size_; }
  // Key slots taken: live entries plus deleted ones still waiting out a
  // reader grace period in the QSBR defer queue.
  size_t slots_in_use() const noexcept {
    return static_cast<size_t>(rte_hash_count(table_));
  }
  uint32_t capacity() const noexcept { return capacity_; }
  uint32_t key_len() const noexcept { return key_len_; }

  // -- reader -------------------------------------------------------------------

  // Looks up `n` (<= 64) keys packed at `stride` bytes. Writes values[i] and
  // sets bit i for each hit; misses leave values[i] untouched.
  uint64_t LookupBatch(ConstBytes keys, size_t stride, uint64_t *values,
                       size_t n) const noexcept {
    promise(n <= 64);
    const void *ptrs[64];
    void *data[64];
    for (size_t i = 0; i < n; i++) {
      ptrs[i] = keys.data() + i * stride;
    }
    uint64_t hits = 0;
    rte_hash_lookup_bulk_data(table_, ptrs, static_cast<uint32_t>(n), &hits,
                              data);
    for (uint64_t m = hits; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      values[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data[i]));
    }
    return hits;
  }

 private:
  ConcurrentExactTable(rte_hash *table, uint32_t key_len, uint32_t capacity)
      : table_(table), key_len_(key_len), capacity_(capacity) {}

  rte_hash *table_;
  const uint32_t key_len_;
  const uint32_t capacity_;
  size_t size_ = 0;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_CONCURRENT_EXACT_H_
