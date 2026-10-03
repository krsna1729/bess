// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_MEMBER_SELECT_H_
#define BESS_DATAPLANE_MEMBER_SELECT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace bess::dataplane {

// Member-selection algorithms (M16, D-066): low-level pieces that ECMP (the
// Router's next-hop groups), load balancers (HashLB) and other group choices
// share, without a common group object. Each selector maps a 32-bit hash to a
// member index in [0, n). The pure ones (all but RoundRobinCursor) are
// immutable once built: build on the control side, publish, read anywhere.
//
//   RangeSelect / RangeSelector   floor(hash * n / 2^32); no table; a change
//                                 of n moves most flows
//   WeightedSelector              weighted choice (alias table), O(1)
//   MaglevSelector                consistent hash (Maglev table), O(1);
//                                 removing a member moves few other flows
//   RendezvousSelector            highest random weight, O(n); removing a
//                                 member moves only that member's flows
//   RoundRobinCursor              mutable, one owner, no hash
//
// A selector known at compile time is a policy type and inlines. One chosen at
// run time is an AnySelector, which switches once per batch (SelectBatch), not
// per packet or per member.

// -- range ------------------------------------------------------------------------

// Lemire's multiply-shift: uniform over [0, n) for a uniform hash, no
// division. Identical to HashLB's former floating-point mapping and to the
// Router's group member choice.
constexpr uint32_t RangeSelect(uint32_t hash, uint32_t n) noexcept {
  return static_cast<uint32_t>((uint64_t{hash} * n) >> 32);
}

struct RangeSelector {
  uint32_t n = 0;
  uint32_t operator()(uint32_t hash) const noexcept { return RangeSelect(hash, n); }
  size_t size() const noexcept { return n; }
  size_t memory_bytes() const noexcept { return sizeof(*this); }
};

// -- weighted ---------------------------------------------------------------------

// Walker/Vose alias method with exact integer thresholds: member i is chosen
// with probability weights[i] / sum, for a uniform hash, to within about 2^-32
// per bucket; a member's error grows with the number of buckets aliased to it
// (at most n), so it is up to about 2 n 2^-32. The hash
// picks a bucket by multiply-shift, and the fractional part of the same
// product (the low 32 bits) decides between the bucket and its alias, so one
// 32-bit hash suffices. Two loads a selection.
class WeightedSelector {
 public:
  // nullopt for no members, all-zero weights, or more than 2^24 members.
  static std::optional<WeightedSelector> Build(std::span<const uint32_t> weights) {
    const size_t n = weights.size();
    uint64_t total = 0;
    for (const uint32_t w : weights) {
      total += w;
    }
    if (n == 0 || total == 0 || n > (size_t{1} << 24)) {
      return std::nullopt;
    }
    WeightedSelector s;
    s.threshold_.assign(n, 0);
    s.alias_.resize(n);
    // Scaled weights: w_i * n against `total`, the bucket's full height.
    std::vector<uint64_t> scaled(n);
    std::vector<uint32_t> small, large;
    for (size_t i = 0; i < n; i++) {
      scaled[i] = uint64_t{weights[i]} * n;
      (scaled[i] < total ? small : large).push_back(static_cast<uint32_t>(i));
      s.alias_[i] = static_cast<uint32_t>(i);
    }
    while (!small.empty() && !large.empty()) {
      const uint32_t l = small.back();
      small.pop_back();
      const uint32_t g = large.back();
      large.pop_back();
      // Bucket l keeps scaled[l] / total of its height; the rest goes to g.
      s.threshold_[l] = static_cast<uint32_t>(
          (static_cast<unsigned __int128>(scaled[l]) << 32) / total);
      s.alias_[l] = g;
      scaled[g] -= total - scaled[l];
      (scaled[g] < total ? small : large).push_back(g);
    }
    // Leftovers are full buckets (rounding leaves at most tiny remainders):
    // alias to themselves, so the threshold does not matter.
    for (const uint32_t i : small) {
      s.alias_[i] = i;
    }
    for (const uint32_t i : large) {
      s.alias_[i] = i;
    }
    return s;
  }

  uint32_t operator()(uint32_t hash) const noexcept {
    const uint64_t product = uint64_t{hash} * threshold_.size();
    const auto bucket = static_cast<uint32_t>(product >> 32);
    // Branch-free: the coin is a fair flip per hash, so a branch on it
    // mispredicts about half the time (measured: 7 ns against 1 ns for range).
    const uint32_t keep =
        0u - static_cast<uint32_t>(static_cast<uint32_t>(product) < threshold_[bucket]);
    return (bucket & keep) | (alias_[bucket] & ~keep);
  }

  size_t size() const noexcept { return threshold_.size(); }
  size_t memory_bytes() const noexcept {
    return sizeof(*this) + threshold_.size() * sizeof(uint32_t) * 2;
  }
  // For tests: bucket i keeps threshold(i) / 2^32 of its share, the rest is alias(i)'s.
  uint32_t threshold(size_t i) const noexcept { return threshold_[i]; }
  uint32_t alias(size_t i) const noexcept { return alias_[i]; }

 private:
  std::vector<uint32_t> threshold_;
  std::vector<uint32_t> alias_;
};

// -- consistent hash (Maglev) ----------------------------------------------------

namespace select_internal {
// splitmix64's finaliser.
constexpr uint64_t Mix64(uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}
}  // namespace select_internal

// Maglev (Eisenbud et al., NSDI 2016): a lookup table of `table_size` entries
// (a prime, ideally >= 100 x members) that every member fills in turn along its
// own permutation. Members get table_size / n entries, +-1 for n << size; the
// permutation of a member depends only on its key, so removing one member
// reassigns its entries and only a few others (measured in D-066). Member keys
// are stable identities (a next-hop id, a backend address), not indices.
class MaglevSelector {
 public:
  static constexpr uint32_t kDefaultTableSize = 65537;  // prime

  // nullopt for no members, more than 65,535, duplicate keys, or a table size
  // that is not a prime larger than the member count.
  static std::optional<MaglevSelector> Build(std::span<const uint64_t> member_keys,
                                             uint32_t table_size = kDefaultTableSize) {
    const size_t n = member_keys.size();
    if (n == 0 || n > 65535 || table_size <= n || !IsPrime(table_size)) {
      return std::nullopt;
    }
    std::vector<uint64_t> sorted(member_keys.begin(), member_keys.end());
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      return std::nullopt;
    }
    std::vector<uint32_t> offset(n), skip(n), next(n, 0);
    for (size_t i = 0; i < n; i++) {
      const uint64_t h = select_internal::Mix64(member_keys[i]);
      offset[i] = static_cast<uint32_t>((h >> 32) % table_size);
      skip[i] = static_cast<uint32_t>((h & 0xffffffffu) % (table_size - 1)) + 1;
    }
    MaglevSelector s;
    s.table_.assign(table_size, kEmpty);
    size_t filled = 0;
    while (true) {
      for (size_t i = 0; i < n; i++) {
        uint32_t c = static_cast<uint32_t>((offset[i] + uint64_t{next[i]} * skip[i]) % table_size);
        while (s.table_[c] != kEmpty) {
          next[i]++;
          c = static_cast<uint32_t>((offset[i] + uint64_t{next[i]} * skip[i]) % table_size);
        }
        s.table_[c] = static_cast<uint16_t>(i);
        next[i]++;
        if (++filled == table_size) {
          s.members_ = static_cast<uint32_t>(n);
          return s;
        }
      }
    }
  }

  uint32_t operator()(uint32_t hash) const noexcept {
    return table_[RangeSelect(hash, static_cast<uint32_t>(table_.size()))];
  }

  size_t size() const noexcept { return members_; }
  size_t table_size() const noexcept { return table_.size(); }
  size_t memory_bytes() const noexcept { return sizeof(*this) + table_.size() * sizeof(uint16_t); }
  uint32_t entry(size_t i) const noexcept { return table_[i]; }

 private:
  static constexpr uint16_t kEmpty = 0xffff;
  static bool IsPrime(uint32_t v) {
    if (v < 2) return false;
    for (uint32_t d = 2; uint64_t{d} * d <= v; d++) {
      if (v % d == 0) return false;
    }
    return true;
  }
  std::vector<uint16_t> table_;
  uint32_t members_ = 0;
};

// -- rendezvous (highest random weight) -------------------------------------------

// The member whose mix(hash, key) is highest. O(n) a selection, no table;
// removing a member moves exactly the flows that were on it. Member keys are
// stable identities, as for Maglev.
class RendezvousSelector {
 public:
  static std::optional<RendezvousSelector> Build(std::span<const uint64_t> member_keys) {
    if (member_keys.empty() || member_keys.size() > (size_t{1} << 24)) {
      return std::nullopt;
    }
    RendezvousSelector s;
    s.keys_.reserve(member_keys.size());
    for (const uint64_t k : member_keys) {
      s.keys_.push_back(select_internal::Mix64(k));
    }
    return s;
  }

  uint32_t operator()(uint32_t hash) const noexcept {
    uint64_t best = 0;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < keys_.size(); i++) {
      const uint64_t score = select_internal::Mix64(keys_[i] ^ hash);
      if (score > best) {
        best = score;
        chosen = i;
      }
    }
    return chosen;
  }

  size_t size() const noexcept { return keys_.size(); }
  size_t memory_bytes() const noexcept { return sizeof(*this) + keys_.size() * sizeof(uint64_t); }

 private:
  std::vector<uint64_t> keys_;
};

// -- round robin --------------------------------------------------------------------

// A mutable cursor, unlike the hash selectors: one owner (a worker, or a
// control thread) calls Next; it is not thread-safe, and two workers sharing a
// group each keep their own cursor (D-066). Next(n) returns 0..n-1 in turn and
// restarts at 0 when n shrinks below the cursor.
class RoundRobinCursor {
 public:
  uint32_t Next(uint32_t n) noexcept {
    if (n == 0) {
      return 0;
    }
    const uint32_t chosen = next_ < n ? next_ : 0;
    next_ = chosen + 1 == n ? 0 : chosen + 1;
    return chosen;
  }

 private:
  uint32_t next_ = 0;
};

// -- runtime choice -----------------------------------------------------------------

enum class SelectorKind : uint8_t { kRange, kWeighted, kMaglev, kRendezvous };

// One of the pure selectors, chosen at run time (a module argument, a
// controller). SelectBatch dispatches once and runs the chosen selector's
// inlined loop over the batch.
class AnySelector {
 public:
  template <typename S>
  explicit AnySelector(S selector) : selector_(std::move(selector)) {}

  SelectorKind kind() const noexcept { return static_cast<SelectorKind>(selector_.index()); }

  void SelectBatch(std::span<const uint32_t> hashes, std::span<uint32_t> out) const noexcept {
    std::visit(
        [&](const auto &s) {
          for (size_t i = 0; i < hashes.size(); i++) {
            out[i] = s(hashes[i]);
          }
        },
        selector_);
  }
  uint32_t Select(uint32_t hash) const noexcept {
    return std::visit([hash](const auto &s) { return s(hash); }, selector_);
  }
  size_t size() const noexcept {
    return std::visit([](const auto &s) { return s.size(); }, selector_);
  }

 private:
  // Order matches SelectorKind.
  std::variant<RangeSelector, WeightedSelector, MaglevSelector, RendezvousSelector> selector_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_MEMBER_SELECT_H_
