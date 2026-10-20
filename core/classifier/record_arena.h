// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_RECORD_ARENA_H_
#define BESS_CLASSIFIER_RECORD_ARENA_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "classifier/classifier.h"
#include "rcu/rcu_domain.h"

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
  // Reclaims ids whose grace period completed.
  [[nodiscard]] Id Allocate();

  // Writer access to an allocated id, before it is published. Id 0 or an
  // out-of-range id is a programming error (empty span).
  [[nodiscard]] MutableBytes Record(Id id) noexcept;

  // Reader access. Id 0 or an out-of-range id returns an empty span.
  [[nodiscard]] ConstBytes Lookup(Id id) const noexcept;

  // The record was never published: free it now.
  void Abort(Id id) noexcept;

  // The record is no longer reachable from any table: free it after the
  // current grace period.
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
  [[nodiscard]] std::byte *Base(Id id) const noexcept;

  const size_t record_size_;
  const size_t stride_;
  rcu::RcuDomain &domain_;
  // Chunk storage is stable: push_back moves the unique_ptr, never the bytes.
  std::vector<Chunk> chunks_;
  // Published chunk bases, so a reader reaches one with an acquire load.
  std::unique_ptr<std::atomic<const std::byte *>[]> published_;
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
