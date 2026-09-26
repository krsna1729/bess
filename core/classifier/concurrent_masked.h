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


#ifndef BESS_CLASSIFIER_CONCURRENT_MASKED_H_
#define BESS_CLASSIFIER_CONCURRENT_MASKED_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "classifier/backend.h"
#include "classifier/concurrent_exact.h"
#include "classifier/masked_exact.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

namespace bess::classifier {

// A masked (ternary) table updated in place while workers read it (G1.2
// mode C): tuple-space search where every tuple -- one per distinct mask --
// is a ConcurrentExactTable keyed by the masked value.
//
//   tuple list (RcuPtr, republished only when a mask appears or goes away)
//     ├── mask A -> ConcurrentExactTable: masked value -> rule id
//     └── mask B -> ...
//   rule records (write-once, id-indexed): {priority, sequence, result}
//
// A table value packs the rule id (low 32 bits) with the rule's result (bits
// 32-47), so a packet that exactly one tuple matches -- the common case --
// takes its result from the table without touching the record; records are
// loaded only to rank a second match for the same packet.
//
// A lookup masks the batch once per tuple, looks it up, and keeps the best
// rule per packet: higher priority, then the later command (sequence). Adding
// the same (mask, value) again replaces the rule, and the replacement counts
// as the later command -- the semantics WildcardMatch promises.
//
// Rule records are write-once. Changing a rule writes a new record under a
// new id and swaps the table value to it atomically; the old id is recycled
// only after a grace period on the runtime RcuDomain, because a reader may
// have loaded it just before the swap (D-013: a shared table needs a grace
// period before memory is reused). Deleting a rule retires its id the same
// way. Records live in fixed chunks that are never moved or freed while the
// table lives, so a reader's record load is a plain indexed load.
//
// One writer (the caller serializes); lock-free readers that are RcuDomain
// readers (workers are). Values and masks must be canonical: value & ~mask
// == 0.
class ConcurrentMaskedTable {
 public:
  using RuleId = uint32_t;  // 1-based; 0 is never a rule

  struct Rule {
    int64_t priority;
    uint64_t sequence;  // command order: ties go to the later command
    uint16_t result;
  };

  enum class UpsertResult : uint8_t {
    kInserted,
    kUpdated,
    kTooManyTuples,  // a new mask beyond max_tuples
    kFull,           // out of rule ids, or a tuple table could not grow
    kNotCanonical,
  };

  static constexpr size_t kMaxBatch = 64;
  static constexpr uint32_t kChunkBits = 10;  // 1024 records per chunk
  static constexpr uint32_t kMaxChunks = 1u << 14;  // 16M rules

  static std::expected<std::unique_ptr<ConcurrentMaskedTable>, std::string>
  Create(uint32_t key_len, size_t max_tuples, rcu::RcuDomain &domain);

  ~ConcurrentMaskedTable();

  ConcurrentMaskedTable(const ConcurrentMaskedTable &) = delete;
  ConcurrentMaskedTable &operator=(const ConcurrentMaskedTable &) = delete;

  // -- writer ------------------------------------------------------------------

  UpsertResult Upsert(ConstBytes mask, ConstBytes value, int64_t priority,
                      uint16_t result);
  // False if no rule has exactly this (mask, value).
  bool Erase(ConstBytes mask, ConstBytes value);
  // Removes every rule and every tuple.
  void Clear();

  // Visits every rule: fn(ConstBytes mask, ConstBytes value, const Rule &).
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    for (const Tuple &tuple : tuples_.Read()->tuples) {
      tuple.table->ForEach([&](ConstBytes value, uint64_t packed) {
        fn(ConstBytes(tuple.mask.data(), tuple.mask.size()), value,
           Record(IdOf(packed)));
      });
    }
  }

  size_t size() const noexcept { return size_; }
  size_t tuple_count() const noexcept { return tuples_.Read()->tuples.size(); }
  size_t max_tuples() const noexcept { return max_tuples_; }
  uint32_t key_len() const noexcept { return key_len_; }
  // Rule ids retired but not yet past their grace period.
  size_t retiring_ids() const noexcept { return retiring_.size(); }

  // -- reader -------------------------------------------------------------------

  // Classifies `n` (<= kMaxBatch) keys packed at `stride` bytes. Writes
  // results[i] and sets bit i for each key some rule matches; others are left
  // untouched.
  uint64_t LookupBatch(ConstBytes keys, size_t stride, uint16_t *results,
                       size_t n) const noexcept {
    promise(n <= kMaxBatch);
    const TupleList *list = tuples_.Read();
    if (n == 0 || list->tuples.empty()) {
      return 0;
    }
    std::array<std::byte, kMaxBatch * detail::kMaskedMaxKeyBytes> scratch;
    std::array<uint64_t, kMaxBatch> ids;
    std::array<uint64_t, kMaxBatch> best;  // packed table value
    const size_t scratch_bytes = n * key_len_;
    uint64_t matched = 0;
    for (const Tuple &tuple : list->tuples) {
      mask_batch_(keys, stride, ConstBytes(tuple.mask.data(), key_len_), n,
                  MutableBytes(scratch).first(scratch_bytes));
      const uint64_t hits = tuple.table->LookupBatch(
          ConstBytes(scratch).first(scratch_bytes), key_len_, ids.data(), n);
      for (uint64_t m = hits; m != 0; m &= m - 1) {
        const size_t i = static_cast<size_t>(__builtin_ctzll(m));
        const uint64_t bit = uint64_t{1} << i;
        if ((matched & bit) == 0) {
          best[i] = ids[i];
          matched |= bit;
        } else if (Better(Record(IdOf(ids[i])), Record(IdOf(best[i])))) {
          best[i] = ids[i];
        }
      }
    }
    for (uint64_t m = matched; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      results[i] = static_cast<uint16_t>(best[i] >> 32);
    }
    return matched;
  }

 private:
  struct Tuple {
    std::vector<std::byte> mask;
    std::shared_ptr<ConcurrentExactTable> table;
  };
  struct TupleList {
    std::vector<Tuple> tuples;
  };
  struct Retiring {
    RuleId id;
    rcu::GracePeriod token;
  };

  ConcurrentMaskedTable(uint32_t key_len, size_t max_tuples,
                        rcu::RcuDomain &domain);

  static RuleId IdOf(uint64_t value) noexcept {
    return static_cast<RuleId>(value);
  }
  static uint64_t Pack(RuleId id, uint16_t result) noexcept {
    return uint64_t{id} | (uint64_t{result} << 32);
  }

  static bool Better(const Rule &candidate, const Rule &incumbent) noexcept {
    return candidate.priority != incumbent.priority
               ? candidate.priority > incumbent.priority
               : candidate.sequence > incumbent.sequence;
  }

  const Rule &Record(RuleId id) const noexcept {
    const Rule *chunk =
        chunks_[id >> kChunkBits].load(std::memory_order_acquire);
    return chunk[id & ((1u << kChunkBits) - 1)];
  }
  Rule &MutableRecord(RuleId id);

  // Writer helpers.
  RuleId AllocateId();         // 0 when out of ids
  void RetireId(RuleId id);
  void ReclaimIds();
  std::shared_ptr<ConcurrentExactTable> NewTupleTable(size_t rules) const;
  // A copy of `from` at least twice its capacity; nullptr on failure.
  std::shared_ptr<ConcurrentExactTable> Grown(
      const ConcurrentExactTable &from) const;
  // Publishes `list` and reclaims what readers are done with.
  void PublishTuples(std::unique_ptr<TupleList> list);
  // The rule id stored for `value` in `table`, or 0.
  static RuleId Find(const ConcurrentExactTable &table, ConstBytes value);

  const uint32_t key_len_;
  const size_t max_tuples_;
  rcu::RcuDomain &domain_;
  detail::MaskBatchFn mask_batch_;
  rcu::RcuPtr<TupleList> tuples_;
  std::unique_ptr<std::atomic<Rule *>[]> chunks_;  // kMaxChunks entries
  std::vector<std::unique_ptr<Rule[]>> chunk_storage_;
  RuleId next_id_ = 1;
  uint64_t next_sequence_ = 1;
  std::vector<RuleId> free_ids_;
  std::deque<Retiring> retiring_;
  size_t size_ = 0;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_CONCURRENT_MASKED_H_
