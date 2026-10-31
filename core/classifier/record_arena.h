// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_RECORD_ARENA_H_
#define BESS_CLASSIFIER_RECORD_ARENA_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "classifier/classifier.h"
#include "rcu/rcu_domain.h"
#include "utils/dpdk_memory.h"

namespace bess::classifier {

// A fixed-width, write-once record store addressed by a 1-based id.
//
// One control-plane writer allocates and fills records; packet readers look
// one up with a single acquire load of the chunk pointer and no lock. Records
// live in chunks that are never moved or freed while the arena lives, so a
// reader's load is a plain indexed load.
//
// An id must not be reused while a reader may still hold it: the owner swaps
// the reference (a table value, a rule record) first, then Retire()s the old
// id, which returns it to the free list only after a grace period on `domain`.
// Abort() is for an id that was never published, so no reader can have seen
// it; it is free immediately.
class RecordArena {
 public:
  using Id = uint32_t;  // 1-based; 0 is never a record

  static constexpr size_t kMaxRecordSize = 4096;
  static constexpr uint32_t kChunkBits = 10;        // 1024 records per chunk
  static constexpr uint32_t kMaxChunks = 1U << 14U;  // 16M records

  RecordArena(size_t record_size, rcu::RcuDomain &domain);
  ~RecordArena();

  RecordArena(const RecordArena &) = delete;
  RecordArena &operator=(const RecordArena &) = delete;

  // A free id, or 0 when the arena is exhausted (or the record size invalid).
  // Reclaims ids whose grace period completed. A new chunk is admitted
  // through the growth hook first (D-198): throws MemoryRefused (or
  // bad_alloc) and allocates nothing when it is refused.
  [[nodiscard]] Id Allocate();

  // Writer access to an allocated id, before it is published. Id 0 or an
  // out-of-range id is a programming error (empty span).
  [[nodiscard]] MutableBytes Record(Id id) noexcept;

  // Reader access. Id 0 or an out-of-range id returns an empty span.
  [[nodiscard, gnu::always_inline]] inline ConstBytes Lookup(Id id) const noexcept {
    const std::byte *base = Base(id);
    if (base == nullptr) {
      return {};
    }
    return ConstBytes(base, record_size_);
  }
  // The record was never published: free it now.
  void Abort(Id id) noexcept;

  // The record is no longer reachable from any table: free it after the
  // current grace period. Reclaims ids whose grace period completed.
  void Retire(Id id);

  // Grow the retirement queue now, on the control path, so a later Retire()
  // from noexcept publication cannot allocate (D-172). Throws bad_alloc; the
  // caller rejects the transaction before anything is visible. A transaction
  // that reserves and then aborts must call CancelRetirement().
  void ReserveRetirement(size_t count);
  void CancelRetirement(size_t count) noexcept;

  [[nodiscard]] size_t record_size() const noexcept { return record_size_; }
  // Allocated ids that are not free: live, retiring and prepared records.
  [[nodiscard]] size_t in_use() const noexcept {
    return (next_id_ - 1) - free_count_;
  }
  [[nodiscard]] size_t retiring() const noexcept {
    return retiring_.size() - retiring_head_;
  }

  // Memory (D-198). The constructor allocates the published-chunk array,
  // kMaxChunks pointers on the DPDK heap, whatever the record size; records
  // come in chunks of 1024 (ChunkBytes), added by Allocate on the control
  // path and kept until the arena is destroyed.
  static size_t Bytes(size_t record_size, const rcu::RcuDomain &domain);
  // One chunk: 1024 records at the record size rounded up to 16 bytes.
  static size_t ChunkBytes(size_t record_size) noexcept;
  // DPDK memory held: the published array and every chunk.
  [[nodiscard]] size_t bytes() const noexcept {
    return published_.size() * sizeof(published_[0]) +
           chunks_.size() * (stride_ << kChunkBits);
  }
  // Allocate admits each new chunk through `growth` (nullptr: unadmitted).
  void set_memory_growth(bess::utils::MemoryGrowth *growth) noexcept {
    growth_ = growth;
  }

 private:
  struct ChunkDeleter {
    void operator()(std::byte *p) const noexcept;
  };
  using Chunk = std::unique_ptr<std::byte[], ChunkDeleter>;

  void Reclaim();
  // Puts an id that no reader can reach back on the free list. The link lives
  // in the record's own (aligned) bytes, so this path never allocates: Abort()
  // is called from noexcept rollback code.
  void Free(Id id) noexcept;
  [[nodiscard, gnu::always_inline]] inline const std::byte *Base(
      Id id) const noexcept {
    const uint32_t chunk = id >> kChunkBits;
    if (id == 0 || chunk >= kMaxChunks) {
      return nullptr;
    }
    const std::byte *base = published_[chunk].load(std::memory_order_acquire);
    if (base == nullptr) {
      return nullptr;
    }
    const uint32_t offset = id & ((1u << kChunkBits) - 1);
    return base + static_cast<size_t>(offset) * stride_;
  }
  [[nodiscard, gnu::always_inline]] inline std::byte *Base(Id id) noexcept {
    return const_cast<std::byte *>(std::as_const(*this).Base(id));
  }

  const size_t record_size_;
  const size_t stride_;
  rcu::RcuDomain &domain_;
  // Chunk storage is stable: push_back moves the unique_ptr, never the bytes.
  std::vector<Chunk> chunks_;
  // Published chunk bases, so a reader reaches one with an acquire load.
  bess::utils::DpdkArray<std::atomic<const std::byte *>> published_;
  bess::utils::MemoryGrowth *growth_ = nullptr;
  // Free ids are a LIFO list threaded through each record's first bytes.
  Id free_head_ = 0;
  size_t free_count_ = 0;
  struct Retiring {
    Id id;
    rcu::GracePeriod token;
  };
  std::vector<Retiring> retiring_;
  size_t retiring_head_ = 0;
  // Reservations not yet consumed by Retire(): capacity already promised, so
  // a second reservation cannot count the same spare slot twice.
  size_t retirement_reserved_ = 0;
  Id next_id_ = 1;  // the next id never allocated
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RECORD_ARENA_H_
