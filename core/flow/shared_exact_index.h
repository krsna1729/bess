// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_SHARED_EXACT_INDEX_H_
#define BESS_FLOW_SHARED_EXACT_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include <rte_hash.h>

#include "utils/common.h"

namespace bess::rcu {
class RcuDomain;
}  // namespace bess::rcu
namespace bess::classifier {
class ConcurrentExactTable;
}  // namespace bess::classifier

namespace bess::flow {

// Fixed-width key bytes -> 64-bit value, read lock-free by every worker and
// written by one thread at a time: SharedFlowTable's directory (D-028). It is
// the boundary that keeps the backend (ConcurrentExactTable over rte_hash,
// internal) out of the installed headers (consolidation review, 2026-10-04),
// so SharedFlowTable can be installed. The read path is inline and is the
// backend's own (its batch hash, then DPDK's prehashed bulk lookup on the same
// rte_hash), so a lookup costs what it did before the boundary (measured:
// out of line, scalar lookups were 5-14% slower); the writer is out of line.
//
// Deleted keys wait out a grace period of `domain` before their place is
// reused (the backend's protocol); `keys` includes that headroom.
class SharedExactIndex {
 public:
  enum class InsertStatus : uint8_t { kInserted, kExists, kFull };

  // An index for at least `keys` keys of `key_len` bytes on `socket` (DPDK
  // socket id, or -1 for any); null if it cannot be created.
  static std::unique_ptr<SharedExactIndex> Create(uint32_t key_len, size_t keys,
                                                  rcu::RcuDomain &domain, int socket);
  ~SharedExactIndex();
  SharedExactIndex(const SharedExactIndex &) = delete;
  SharedExactIndex &operator=(const SharedExactIndex &) = delete;

  // Readers, any thread. Looks up `n` (<= 64) keys laid out `stride` bytes
  // apart; values[i] is set and bit i of the result is set for each hit.
  uint64_t LookupBatch(const std::byte *keys, size_t stride, uint64_t *values,
                       size_t n) const noexcept {
    promise(n <= 64);
    const void *ptrs[64];
    hash_sig_t sigs[64];
    void *data[64];
    for (size_t i = 0; i < n; i++) {
      ptrs[i] = keys + i * stride;
    }
    hash_batch_(keys, stride, n, sigs);
    uint64_t hits = 0;
    rte_hash_lookup_with_hash_bulk_data(table_dpdk_, ptrs, sigs, static_cast<uint32_t>(n), &hits,
                                        data);
    for (uint64_t m = hits; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      values[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data[i]));
    }
    return hits;
  }

  // One key: true and *value set on a hit.
  bool Lookup(const std::byte *key, uint64_t *value) const noexcept {
    return LookupBatch(key, key_len_, value, 1) != 0;
  }

  // The writer (one thread at a time).
  InsertStatus InsertIfAbsent(const std::byte *key, uint64_t value);
  bool Erase(const std::byte *key);
  // Reuses every deleted key's place; no reader may be online.
  void ReclaimAll();

  size_t size() const noexcept;

  // Tests only (they include the internal backend header): the backend, to
  // place keys deliberately.
  const classifier::ConcurrentExactTable &backend_for_testing() const noexcept { return *table_; }

 private:
  SharedExactIndex(std::unique_ptr<classifier::ConcurrentExactTable> table,
                   uint32_t key_len) noexcept;

  // The batch hash of the backend: a fixed-width kernel chosen at create.
  using HashBatchFn = void (*)(const std::byte *keys, size_t stride, size_t n, hash_sig_t *sigs);

  std::unique_ptr<classifier::ConcurrentExactTable> table_;
  rte_hash *table_dpdk_;
  HashBatchFn hash_batch_;
  uint32_t key_len_;
};

}  // namespace bess::flow

#endif  // BESS_FLOW_SHARED_EXACT_INDEX_H_
