// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_RANGE_BACKEND_H_
#define BESS_CLASSIFIER_RANGE_BACKEND_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "classifier/byte_key.h"
#include "utils/common.h"
#include "utils/endian.h"
#include "utils/logging.h"

namespace bess::classifier {

// Closed interval [low, high] for port matching (inclusive).
struct PortRange {
  uint16_t low = 0;
  uint16_t high = 0xffff;

  constexpr bool operator==(const PortRange &) const = default;

  [[nodiscard]] constexpr bool Contains(uint16_t val) const noexcept {
    return val >= low && val <= high;
  }

  [[nodiscard]] constexpr bool IsWildcard() const noexcept {
    return low == 0 && high == 0xffff;
  }

  [[nodiscard]] constexpr bool IsExact() const noexcept {
    return low == high;
  }
};

template <typename Result, typename Priority = int64_t>
struct RangeRule {
  std::vector<std::byte> value;
  std::vector<std::byte> mask;
  PortRange src_range{};
  PortRange dst_range{};
  size_t src_port_offset = 0;
  size_t dst_port_offset = 0;
  Priority priority{};
  Result result{};
};

// Golden scalar reference implementation for differential testing and small rule sets.
template <typename Result, typename Priority = int64_t>
class ScalarRangeBackend {
 public:
  struct RuleEntry {
    std::vector<std::byte> value;
    std::vector<std::byte> mask;
    PortRange src_range;
    PortRange dst_range;
    size_t src_port_offset;
    size_t dst_port_offset;
    Priority priority;
    Result result;
  };

  ScalarRangeBackend() = default;

  explicit ScalarRangeBackend(std::vector<RangeRule<Result, Priority>> rules) {
    rules_.reserve(rules.size());
    for (auto &r : rules) {
      rules_.push_back(RuleEntry{
          .value = std::move(r.value),
          .mask = std::move(r.mask),
          .src_range = r.src_range,
          .dst_range = r.dst_range,
          .src_port_offset = r.src_port_offset,
          .dst_port_offset = r.dst_port_offset,
          .priority = r.priority,
          .result = r.result,
      });
    }
    // Sort descending by priority so the first match is always highest priority
    std::stable_sort(rules_.begin(), rules_.end(),
                     [](const RuleEntry &a, const RuleEntry &b) {
                       return a.priority > b.priority;
                     });
  }

  [[nodiscard]] bool MatchRule(const RuleEntry &rule, const std::byte *key,
                               size_t key_size) const noexcept {
    const size_t check_len = std::min(rule.mask.size(), key_size);
    for (size_t i = 0; i < check_len; i++) {
      if ((key[i] & rule.mask[i]) != rule.value[i]) {
        return false;
      }
    }

    if (!rule.src_range.IsWildcard() &&
        rule.src_port_offset + sizeof(uint16_t) <= key_size) {
      bess::utils::be16_t be_port;
      std::memcpy(&be_port, key + rule.src_port_offset, sizeof(uint16_t));
      if (!rule.src_range.Contains(be_port.value())) {
        return false;
      }
    }

    if (!rule.dst_range.IsWildcard() &&
        rule.dst_port_offset + sizeof(uint16_t) <= key_size) {
      bess::utils::be16_t be_port;
      std::memcpy(&be_port, key + rule.dst_port_offset, sizeof(uint16_t));
      if (!rule.dst_range.Contains(be_port.value())) {
        return false;
      }
    }

    return true;
  }

  [[nodiscard]] uint64_t LookupBatch(ConstBytes keys, size_t key_stride,
                                     size_t key_size,
                                     std::span<Result> results) const noexcept {
    const size_t count = std::min(results.size(), size_t{64});
    // The caller's contract: the buffer holds `count` keys, and the last one
    // needs only key_size bytes - its stride padding may be absent. A DCHECK,
    // not a clamp: the batch comes from results.size(), and silently dropping
    // keys would leave the caller's later slots stale. A release CHECK
    // measured +0.53 ns at batch 1 (2.12 -> 2.65) and -2.8 to -4.5 ns at
    // batches 8-32, but the only in-tree caller looks up one key per packet,
    // so the guard is debug-only and the contract is documented instead.
    DCHECK(count == 0 ||
           keys.size() >= (count - 1) * key_stride + key_size);
    uint64_t hit_mask = 0;

    for (size_t i = 0; i < count; i++) {
      const std::byte *key = keys.data() + i * key_stride;
      for (const auto &rule : rules_) {
        if (MatchRule(rule, key, key_size)) {
          results[i] = rule.result;
          hit_mask |= (uint64_t{1} << i);
          break;
        }
      }
    }
    return hit_mask;
  }

  [[nodiscard]] size_t size() const noexcept { return rules_.size(); }

  // Memory (D-198), all glibc: the rule array and each rule's value and
  // mask, which the constructor takes over from `rules` with their capacity
  // (exact when `rules` is moved in; an upper bound when it is copied, as a
  // copy holds only the size). Immutable once built.
  static size_t Bytes(const std::vector<RangeRule<Result, Priority>> &rules) {
    size_t total = rules.size() * sizeof(RuleEntry);
    for (const auto &r : rules) {
      total += r.value.capacity() + r.mask.capacity();
    }
    return total;
  }
  static size_t GlibcBytes(
      const std::vector<RangeRule<Result, Priority>> &rules) {
    return Bytes(rules);
  }
  [[nodiscard]] size_t bytes() const noexcept { return EntriesBytes(rules_); }
  // The bytes held by an array of rule entries and their values and masks.
  static size_t EntriesBytes(const std::vector<RuleEntry> &entries) noexcept {
    size_t total = entries.capacity() * sizeof(RuleEntry);
    for (const RuleEntry &e : entries) {
      total += e.value.capacity() + e.mask.capacity();
    }
    return total;
  }

 private:
  std::vector<RuleEntry> rules_;
};

// K3.8 Range Classifier: vectorized range checks with differential reference validation.
template <typename Result, typename Priority = int64_t>
class RangeClassifier {
 public:
  RangeClassifier() = default;

  explicit RangeClassifier(std::vector<RangeRule<Result, Priority>> rules)
      : reference_(rules) {
    rules_.reserve(rules.size());
    for (auto &r : rules) {
      rules_.push_back(typename ScalarRangeBackend<Result, Priority>::RuleEntry{
          .value = std::move(r.value),
          .mask = std::move(r.mask),
          .src_range = r.src_range,
          .dst_range = r.dst_range,
          .src_port_offset = r.src_port_offset,
          .dst_port_offset = r.dst_port_offset,
          .priority = r.priority,
          .result = r.result,
      });
    }
    std::stable_sort(rules_.begin(), rules_.end(),
                     [](const auto &a, const auto &b) {
                       return a.priority > b.priority;
                     });
  }

  // The mask compare is word-wise here, and the port checks repeat the
  // reference's: the reference stays byte-at-a-time because it is the
  // differential oracle, so the two implementations must stay separate. The
  // byte loop dominated the lookup (K3.8's bench measured 2.1x a hand-written
  // scalar arm), and a mask is an unaligned byte string, so the words are read
  // with memcpy.
  [[nodiscard]] static bool MatchFast(
      const typename ScalarRangeBackend<Result, Priority>::RuleEntry &rule,
      const std::byte *key, size_t key_size) noexcept {
    const size_t check_len = std::min(rule.mask.size(), key_size);
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= check_len; i += sizeof(uint64_t)) {
      uint64_t k, m, v;
      std::memcpy(&k, key + i, sizeof(uint64_t));
      std::memcpy(&m, rule.mask.data() + i, sizeof(uint64_t));
      std::memcpy(&v, rule.value.data() + i, sizeof(uint64_t));
      if ((k & m) != v) {
        return false;
      }
    }
    for (; i < check_len; i++) {
      if ((key[i] & rule.mask[i]) != rule.value[i]) {
        return false;
      }
    }

    if (!rule.src_range.IsWildcard() &&
        rule.src_port_offset + sizeof(uint16_t) <= key_size) {
      bess::utils::be16_t be_port;
      std::memcpy(&be_port, key + rule.src_port_offset, sizeof(uint16_t));
      if (!rule.src_range.Contains(be_port.value())) {
        return false;
      }
    }

    if (!rule.dst_range.IsWildcard() &&
        rule.dst_port_offset + sizeof(uint16_t) <= key_size) {
      bess::utils::be16_t be_port;
      std::memcpy(&be_port, key + rule.dst_port_offset, sizeof(uint16_t));
      if (!rule.dst_range.Contains(be_port.value())) {
        return false;
      }
    }

    return true;
  }

  // Lookup a batch of packets. Guarantees 100% equivalence to the scalar reference.
  [[nodiscard]] uint64_t LookupBatch(ConstBytes keys, size_t key_stride,
                                     size_t key_size,
                                     std::span<Result> results) const noexcept {
    const size_t count = std::min(results.size(), size_t{64});
    // The caller's contract: the buffer holds `count` keys, and the last one
    // needs only key_size bytes - its stride padding may be absent. A DCHECK,
    // not a clamp: the batch comes from results.size(), and silently dropping
    // keys would leave the caller's later slots stale. A release CHECK
    // measured +0.53 ns at batch 1 (2.12 -> 2.65) and -2.8 to -4.5 ns at
    // batches 8-32, but the only in-tree caller looks up one key per packet,
    // so the guard is debug-only and the contract is documented instead.
    DCHECK(count == 0 ||
           keys.size() >= (count - 1) * key_stride + key_size);
    uint64_t hit_mask = 0;

    for (size_t i = 0; i < count; i++) {
      const std::byte *key = keys.data() + i * key_stride;
      for (const auto &rule : rules_) {
        if (MatchFast(rule, key, key_size)) {
          results[i] = rule.result;
          hit_mask |= (uint64_t{1} << i);
          break;
        }
      }
    }
    return hit_mask;
  }

  [[nodiscard]] const ScalarRangeBackend<Result, Priority> &reference() const noexcept {
    return reference_;
  }

  [[nodiscard]] size_t size() const noexcept { return rules_.size(); }

  // Memory (D-198), all glibc: two copies of the rules -- the reference's
  // (copied, so exactly their sizes) and the fast path's, which takes over
  // the values and masks of `rules` with their capacity (exact when `rules`
  // is moved in, an upper bound when copied). Immutable once built.
  static size_t Bytes(const std::vector<RangeRule<Result, Priority>> &rules) {
    using Entry = typename ScalarRangeBackend<Result, Priority>::RuleEntry;
    size_t total = 2 * rules.size() * sizeof(Entry);
    for (const auto &r : rules) {
      total += r.value.size() + r.mask.size() + r.value.capacity() +
               r.mask.capacity();
    }
    return total;
  }
  static size_t GlibcBytes(
      const std::vector<RangeRule<Result, Priority>> &rules) {
    return Bytes(rules);
  }
  [[nodiscard]] size_t bytes() const noexcept {
    return reference_.bytes() +
           ScalarRangeBackend<Result, Priority>::EntriesBytes(rules_);
  }

 private:
  ScalarRangeBackend<Result, Priority> reference_;
  std::vector<typename ScalarRangeBackend<Result, Priority>::RuleEntry> rules_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RANGE_BACKEND_H_
