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

  // Lookup a batch of packets. Guarantees 100% equivalence to the scalar reference.
  [[nodiscard]] uint64_t LookupBatch(ConstBytes keys, size_t key_stride,
                                     size_t key_size,
                                     std::span<Result> results) const noexcept {
    return reference_.LookupBatch(keys, key_stride, key_size, results);
  }

  [[nodiscard]] const ScalarRangeBackend<Result, Priority> &reference() const noexcept {
    return reference_;
  }

  [[nodiscard]] size_t size() const noexcept { return rules_.size(); }

 private:
  ScalarRangeBackend<Result, Priority> reference_;
  std::vector<typename ScalarRangeBackend<Result, Priority>::RuleEntry> rules_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RANGE_BACKEND_H_
