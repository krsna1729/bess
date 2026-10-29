// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_MASKED_EXACT_H_
#define BESS_CLASSIFIER_MASKED_EXACT_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "classifier/backend.h"
#include "classifier/classifier.h"
#include "classifier/cuckoo_exact.h"
#include "classifier/mask_batch.h"
#include "classifier/runtime_schema.h"
#include "dataplane/strong_id.h"
#include "utils/common.h"

namespace bess::classifier {

// One masked (ternary) rule: the key masked by `mask` must equal `value`. The
// canonical form is required, not normalized:
//
//     value & ~mask == 0
//
// A pair violating it is a configuration error and is rejected at build time,
// matching the check the existing WildcardMatch module already performs.
template <typename Result, typename Priority = int64_t>
struct RuntimeMaskedRule {
  ConstBytes value;
  ConstBytes mask;
  Priority priority{};
  Result result{};
};

// Zero is a valid value here: the ID is a direct index into the immutable
// candidate vector, unlike `ActionId`, whose domain reserves zero.
struct MaskedCandidateTag;
using MaskedCandidateId =
    dataplane::StrongId<MaskedCandidateTag, uint32_t>;

// Generation-owned masked classifier: one tuple per distinct mask, each tuple
// owning an exact table keyed by the masked value. Exact tables return a
// generation-owned candidate ID; packet scratch therefore never contains the
// user result object or its rank metadata.
//
//     RuntimeMaskedBackend<Result>
//         ├── Tuple(mask A) → RuntimeExactBackend<MaskedCandidateId>
//         ├── Tuple(mask B) → RuntimeExactBackend<MaskedCandidateId>
//         └── ...
//
// Lookup is tuple-major, batch-minor: mask the batch once per tuple, exact-look
// it up, then merge by rank. The exact machinery is the one the runtime exact
// path already uses — packed keys, borrowed probes, hit masks — so there is no
// second hash table here.
//
// Tuple count is unbounded; the eight-tuple ceiling belongs to the existing
// module's wire compatibility, not to this substrate.
//
// Build contract: construction is control-plane only. Once published the
// backend is immutable, and `lookup_batch()` is safe from concurrent readers.
template <typename Result, typename Priority = int64_t>
class RuntimeMaskedBackend {
  struct Candidate {
    Priority priority;
    uint64_t ordinal;
    Result result;
  };

  using candidate_id = MaskedCandidateId;

 public:

  static constexpr size_t kMaxBatch = 64;

  RuntimeMaskedBackend() = default;
  RuntimeMaskedBackend(const RuntimeMaskedBackend &) = delete;
  RuntimeMaskedBackend &operator=(const RuntimeMaskedBackend &) = delete;
  RuntimeMaskedBackend(RuntimeMaskedBackend &&) = default;
  RuntimeMaskedBackend &operator=(RuntimeMaskedBackend &&) = default;

  // Rejects an empty key size, a key size above `kMaskedMaxKeyBytes`, a rule
  // whose value/mask lengths disagree with the key size, and a rule violating
  // `value & ~mask == 0`.
  // `body` is passed to every tuple's cuckoo backend; with kAuto each tuple
  // chooses from its own footprint (dataplane/batch_tuning.h).
  static ClassifierResult<RuntimeMaskedBackend> Build(
      size_t key_size,
      std::span<const RuntimeMaskedRule<Result, Priority>> rules,
      dataplane::LookupBody body = dataplane::LookupBody::kAuto) {
    if (key_size == 0) {
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kEmptyKey,
          .message = "key size cannot be 0",
      });
    }
    if (key_size > detail::kMaskedMaxKeyBytes) {
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kKeyOutOfBounds,
          .message = "key size exceeds the masked substrate maximum",
      });
    }

    // Group rules by mask, in first-seen order: a tuple is a mask class, not a
    // priority class. Every mask and value is key_size bytes (checked below
    // before use), so they compare as that many bytes. (std::vector's
    // operator<, inlined at -O3, makes GCC 14 report a memcmp bound beyond
    // any object: -Wstringop-overread, a false positive.)
    const auto key_less = [key_size](const std::vector<std::byte> &lhs,
                                     const std::vector<std::byte> &rhs) {
      return std::memcmp(lhs.data(), rhs.data(), key_size) < 0;
    };
    std::map<std::vector<std::byte>, size_t, decltype(key_less)> tuple_of_mask(
        key_less);
    std::vector<std::vector<std::byte>> masks;
    std::vector<std::vector<std::pair<std::vector<std::byte>, Candidate>>>
        entries;

    for (size_t i = 0; i < rules.size(); i++) {
      const auto &rule = rules[i];
      if (rule.value.size() != key_size || rule.mask.size() != key_size) {
        return std::unexpected(ClassifierError{
            .code = ClassifierErrorCode::kInvalidPlan,
            .message = "rule value/mask length does not match the key size",
            .field_index = i,
        });
      }
      if (!IsCanonical(rule)) {
        return std::unexpected(ClassifierError{
            .code = ClassifierErrorCode::kInvalidPlan,
            .message =
                "rule is not canonical: value has bits set outside its mask",
            .field_index = i,
        });
      }

      std::vector<std::byte> mask(rule.mask.begin(), rule.mask.end());
      auto [it, inserted] = tuple_of_mask.emplace(mask, masks.size());
      if (inserted) {
        masks.push_back(std::move(mask));
        entries.emplace_back();
      }
      std::vector<std::byte> value(rule.value.begin(), rule.value.end());
      entries[it->second].emplace_back(
          std::move(value),
          Candidate{.priority = rule.priority,
                    .ordinal = static_cast<uint64_t>(i),
                    .result = rule.result});
    }

    RuntimeMaskedBackend backend;
    backend.key_size_ = key_size;
    backend.mask_batch_ = detail::SelectMaskBatch(key_size);
    backend.candidates_.reserve(rules.size());
    backend.tuples_.reserve(masks.size());

    size_t effective_rules = 0;
    for (size_t t = 0; t < masks.size(); t++) {
      auto &group = entries[t];
      // Equal masked values inside one tuple are the same lookup key. Keep the
      // better rank deterministically: the exact builder rejects duplicate
      // keys, and leaving the choice to insertion order would make the outcome
      // depend on rule order in a way this substrate does not promise.
      std::sort(group.begin(), group.end(),
                [&key_less](const auto &lhs, const auto &rhs) {
                  return key_less(lhs.first, rhs.first);
                });
      std::vector<std::pair<std::vector<std::byte>, Candidate>> deduped;
      deduped.reserve(group.size());
      for (auto &entry : group) {
        // Sorted, so the last kept is not after this one: equal unless less.
        if (!deduped.empty() && !key_less(deduped.back().first, entry.first)) {
          if (BetterRank(entry.second, deduped.back().second)) {
            deduped.back().second = entry.second;
          }
          continue;
        }
        deduped.push_back(std::move(entry));
      }

      std::vector<RuntimeExactRule<candidate_id>> exact_rules;
      exact_rules.reserve(deduped.size());
      for (auto &entry : deduped) {
        if (backend.candidates_.size() >
            std::numeric_limits<typename candidate_id::rep_type>::max()) {
          return std::unexpected(ClassifierError{
              .code = ClassifierErrorCode::kResultOutOfBounds,
              .message = "masked candidate ID space exhausted",
          });
        }
        const candidate_id id(static_cast<typename candidate_id::rep_type>(
            backend.candidates_.size()));
        backend.candidates_.push_back(std::move(entry.second));
        exact_rules.push_back(RuntimeExactRule<candidate_id>{
            .key = ConstBytes(entry.first.data(), entry.first.size()),
            .result = id,
        });
      }

      auto built =
          BuildRuntimeCuckooBackend<candidate_id>(key_size, exact_rules, body);
      if (!built) {
        return std::unexpected(std::move(built.error()));
      }
      backend.tuples_.push_back(Tuple{std::move(masks[t]), std::move(*built)});
      effective_rules += deduped.size();
    }

    backend.rule_count_ = effective_rules;
    return backend;
  }

  // Packet path. Bit i of the return value is set iff `results[i]` was
  // written. A slot that no tuple matched is left untouched.
  [[nodiscard]] uint64_t lookup_batch(ConstBytes keys, size_t key_stride,
                                      std::span<Result> results) const noexcept {
    const size_t count = results.size();
    if (count == 0 || tuples_.empty()) {
      return 0;
    }
    promise(count <= kMaxBatch);
    promise(key_stride >= key_size_);
    promise(key_stride <= keys.size() / count);

    // Stack bound: kMaxBatch * kMaskedMaxKeyBytes for the masked keys plus two
    // fixed-width candidate-ID buffers. The key half is bounded by the
    // 64-byte key ceiling; the merge half is always 2 * kMaxBatch *
    // sizeof(candidate_id), independent of `Result`.
    std::array<std::byte, kMaxBatch * detail::kMaskedMaxKeyBytes> scratch;
    std::array<candidate_id, kMaxBatch> candidates;
    std::array<candidate_id, kMaxBatch> best;
    const size_t scratch_bytes = count * key_size_;

    uint64_t matched = 0;
    for (const Tuple &tuple : tuples_) {
      mask_batch_(keys, key_stride, ConstBytes(tuple.mask), count,
                  MutableBytes(scratch).first(scratch_bytes));
      const uint64_t hits = tuple.backend.lookup_batch(
          ConstBytes(scratch).first(scratch_bytes), key_size_,
          std::span<candidate_id>(candidates).first(count));
      for (size_t i = 0; i < count; i++) {
        const uint64_t bit = uint64_t{1} << i;
        if ((hits & bit) == 0) {
          continue;
        }
        const Candidate &candidate = candidates_[candidates[i].value()];
        if ((matched & bit) == 0 ||
            BetterRank(candidate, candidates_[best[i].value()])) {
          best[i] = candidates[i];
          results[i] = candidate.result;
          matched |= bit;
        }
      }
    }
    return matched;
  }

  [[nodiscard]] size_t key_size() const noexcept { return key_size_; }

  [[nodiscard]] size_t tuple_count() const noexcept { return tuples_.size(); }

  // Effective stored rule count: duplicate (mask, value) rules collapse to one
  // entry at build time, so this is the number of distinct lookup keys the
  // backend actually holds, not the number of rules submitted.
  [[nodiscard]] size_t rule_count() const noexcept { return rule_count_; }

  [[nodiscard]] MaskedBackendInfo info() const noexcept {
    return MaskedBackendInfo{
        .kind = WildcardBackendKind::kTupleSpace,
        .tuple_count = tuples_.size(),
        .rule_count = rule_count_,
        .key_size = key_size_,
        .result_size = sizeof(Result),
    };
  }

  // Memory (D-198), all glibc: the candidates, the tuples, each tuple's mask
  // and its cuckoo backend. Immutable once built. No Bytes(): each tuple's
  // cuckoo map sizes itself by expanding while Build inserts (on hash
  // collisions too), so the cost is known once built, not from the rules.
  [[nodiscard]] size_t bytes() const noexcept {
    size_t total = candidates_.capacity() * sizeof(Candidate) +
                   tuples_.capacity() * sizeof(Tuple);
    for (const Tuple &tuple : tuples_) {
      total += tuple.mask.capacity() + tuple.backend.bytes();
    }
    return total;
  }

 private:
  struct Tuple {
    std::vector<std::byte> mask;
    RuntimeExactBackend<candidate_id> backend;
  };

  static bool IsCanonical(const RuntimeMaskedRule<Result, Priority> &rule) {
    for (size_t b = 0; b < rule.mask.size(); b++) {
      const uint8_t value = std::to_integer<uint8_t>(rule.value[b]);
      const uint8_t mask = std::to_integer<uint8_t>(rule.mask[b]);
      if ((value & static_cast<uint8_t>(~mask)) != 0) {
        return false;
      }
    }
    return true;
  }

  static bool BetterRank(const Candidate &candidate,
                         const Candidate &incumbent) {
    if (candidate.priority != incumbent.priority) {
      return candidate.priority > incumbent.priority;
    }
    return candidate.ordinal > incumbent.ordinal;
  }

  size_t key_size_ = 0;
  size_t rule_count_ = 0;
  detail::MaskBatchFn mask_batch_ = nullptr;
  std::vector<Candidate> candidates_;
  std::vector<Tuple> tuples_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_MASKED_EXACT_H_
