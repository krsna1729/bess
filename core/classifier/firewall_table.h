// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_FIREWALL_TABLE_H_
#define BESS_CLASSIFIER_FIREWALL_TABLE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bess::classifier {

// The address families a rule or a key can name, as the IP version numbers an
// IPv4 or IPv6 header carries; 0 means "any family" and only a rule can say it
// (a key always has one). These are not the AF_* socket constants (2 and 10 on
// Linux), so do not compare them against a socket's family.
enum class FirewallFamily : uint8_t {
  kAny = 0,
  kIpv4 = 4,
  kIpv6 = 6,
};

// A 16-byte address in network order. An IPv4 address lives in the first four
// bytes; the rest are zero, which is what makes one packed key serve both
// families.
using FirewallAddr = std::array<uint8_t, 16>;

// The canonical firewall key: the 5-tuple plus the family. A module extracts
// it from a packet; the library never looks at a packet.
struct FirewallKey {
  FirewallFamily family = FirewallFamily::kIpv4;
  uint8_t proto = 0;
  FirewallAddr src{};
  FirewallAddr dst{};
  uint16_t src_port = 0;
  uint16_t dst_port = 0;
};

// One rule. An address is a network and a prefix length (0 = any), the
// protocol is exact or 0 for any, and each port is an inclusive range (the
// full range is "any"). `family` is kAny only for a rule with no prefix on
// either side. `action` is the caller's: the library compares rules and
// returns the winner's action untouched, so a module can put a gate, an index
// into its own action table, or a drop marker there.
//
// The list is the policy and its order is precedence: the first rule that
// matches wins. A caller whose own precedence is a number (a UPF's "the lower
// value wins") sorts before Build rather than expecting the library to
// reorder. Build is all-or-nothing - a rule it cannot express refuses the
// whole list - so a caller that applies a policy transactionally never
// half-applies one.
struct FirewallRule {
  FirewallFamily family = FirewallFamily::kIpv4;
  FirewallAddr src_net{};  // Build() masks it to src_len
  uint8_t src_len = 0;     // 0..32 for IPv4, 0..128 for IPv6
  FirewallAddr dst_net{};
  uint8_t dst_len = 0;
  uint8_t proto = 0;  // 0 = any
  uint16_t src_port_low = 0;
  uint16_t src_port_high = 0xffff;
  uint16_t dst_port_low = 0;
  uint16_t dst_port_high = 0xffff;
  uint16_t action = 0;
  // The ActionId a matching packet carries, 0 for none (D-141). The library
  // carries it untouched, like `action`; the module publishes it as packet
  // metadata.
  uint32_t action_id = 0;

  // Derived by Build(), not read from the caller: true when this rule
  // constrains a port, so the packet path skips four comparisons for a
  // prefix-only rule (the common shape of a firewall rule).
  bool checks_ports = false;
};

// A firewall rule set as an immutable table: build it, publish it, look it up
// without locks. The caller owns publication - a module swaps a
// runtime::RcuPtr the way ACL's rule list does - so the library has no RCU,
// no writer path and no locks of its own.
//
// The index is tuple space over the masked fields alone: one group per
// (family, protocol, source prefix length, destination prefix length). **The
// port ranges are not part of the key**: a rule's ports are checked after its
// group's masked key hits, so a range costs no group and no mask - unlike a
// WildcardMatch rule, which must express a range as 2n-2 prefixes per field
// and pays for each of them in its tuple budget (max_tuples, D-102).
//
// A group holds one array of (masked key -> rule) entries sorted by key, so a
// lookup is one binary search per group plus the port comparisons, and the
// groups are ordered by the best rule index they hold so a lookup can stop as
// soon as no later group can win. Rule order is the policy order: the first
// matching rule wins, as a firewall's list does. IPv4 and IPv6 rules live in
// one table and one policy: a family is part of a group's identity, so a v4
// rule can never see a v6 packet, and a rule with no prefix at all (family
// kAny) sees both.
class FirewallTable {
 public:
  struct Match {
    uint16_t action = 0;
    uint32_t rule_id = 0;  // 1-based; 0 is never a rule
  };

  // What the build produced. `groups` is the analogue of WildcardMatch's
  // tuple count, and it must not move when only port ranges change.
  struct Stats {
    size_t rules = 0;
    size_t groups = 0;
    size_t ipv4_rules = 0;
    size_t ipv6_rules = 0;
    size_t any_family_rules = 0;
  };

  // The masked addresses of a group's key, packed compactly: source then
  // destination, each masked to the group's prefix length, and only as many
  // bytes as the family occupies (8 for IPv4, 32 for IPv6). Byte order is the
  // wire's, so comparing two packed keys is a plain memcmp.
  using PackedKey = std::array<uint8_t, 32>;

  // Refuses a rule it cannot express exactly - a prefix length beyond the
  // family's width, a port low bound above its high, a prefix on a rule that
  // names no family - rather than widening the constraint.
  static std::expected<std::unique_ptr<const FirewallTable>, std::string>
  Build(std::vector<FirewallRule> rules);

  // Memory (D-198), all glibc: what Build allocates for `rules` -- the
  // table, its copy of the rules, one group per (family, protocol, prefix
  // lengths) and one entry per rule across the groups (each group reserved
  // to its size). Immutable once built; the build's temporary map is not
  // counted.
  static size_t Bytes(const std::vector<FirewallRule> &rules);
  static size_t GlibcBytes(const std::vector<FirewallRule> &rules) {
    return Bytes(rules);
  }
  size_t bytes() const noexcept;

  // Why Build would refuse `rule`, without naming it (the caller knows which
  // rule it is), or nullopt when the table can express it. Build refuses a
  // list exactly when it refuses one of its rules, so a caller that checks
  // each rule as it arrives knows the list will build.
  static std::optional<std::string> Refusal(const FirewallRule &rule);

  // Classifies `n` (<= 64) keys: sets bit i of the returned mask for each key a
  // rule matched, writes that rule's action into matches[i].action (and, with
  // `with_rule_id`, its id). A key nothing matches is left untouched.
  uint64_t LookupBatch(const FirewallKey *keys, size_t n, Match *matches,
                       bool with_rule_id = true) const noexcept;

  // One key; true when a rule matched.
  bool Lookup(const FirewallKey &key, Match *match) const noexcept;

  const Stats &stats() const noexcept { return stats_; }
  size_t rule_count() const noexcept { return rules_.size(); }

  // False when no rule can judge a packet of `family`: a caller holding a
  // v4-only policy can drop stray v6 traffic without a lookup. Derived at
  // build, so it cannot disagree with the rules.
  bool HasFamily(FirewallFamily family) const noexcept {
    if (stats_.any_family_rules > 0) {
      return true;
    }
    return family == FirewallFamily::kIpv6 ? stats_.ipv6_rules > 0
                                           : stats_.ipv4_rules > 0;
  }

  // The masked address of `addr` under `len` bits of `family`: the only place
  // the two families' widths are written down.
  static FirewallAddr MaskedAddr(const FirewallAddr &addr, uint8_t len,
                                 FirewallFamily family) noexcept;
  static PackedKey Pack(const FirewallAddr &src, uint8_t src_len,
                        const FirewallAddr &dst, uint8_t dst_len,
                        FirewallFamily family) noexcept;

 private:
  friend class FirewallScan;

  struct Entry {
    PackedKey key;
    uint32_t rule;  // index into rules_
  };
  struct Group {
    FirewallFamily family;
    uint8_t proto;  // 0 = any
    uint8_t src_len;
    uint8_t dst_len;
    // How many of the 32 packed bytes this group's family actually uses: 8
    // for IPv4 (two 4-byte addresses), 32 for IPv6, 0 for a group with no
    // prefix at all. Entries of one group share a family, so the unused bytes
    // are zero and comparing only these is the same answer for less work -
    // one eighth the compare for the common v4 policy.
    uint8_t key_bytes;
    // Precomputed masks for this group's two prefix lengths, so the packet
    // path ANDs a register instead of masking bit by bit: `mask64` is the
    // IPv4 pair (source in the low half, destination in the high half, in the
    // packed layout), `mask_src`/`mask_dst` the IPv6 pair. Both say the same
    // thing; the family decides which one is read.
    uint64_t mask64;
    FirewallAddr mask_src;
    FirewallAddr mask_dst;
    // The best (lowest) rule index in this group: the groups are ordered by
    // it, and a group whose best index is worse than the match in hand cannot
    // improve it.
    uint32_t first_rule;
    std::vector<Entry> entries;  // sorted by key, equal keys in rule order
  };

  static bool Matches(const FirewallRule &rule, const FirewallKey &key,
                      const PackedKey &packed) noexcept;
  // The first rule of `group` that matches `key`, or 0.
  uint32_t MatchInGroup(const Group &group, const FirewallKey &key) const
      noexcept;

  FirewallTable() = default;

  std::vector<FirewallRule> rules_;
  std::vector<Group> groups_;  // ordered by first_rule, best first
  Stats stats_;
};

// A scan over a small policy: the rules in policy order, matched with the same
// predicate the indexed table uses, and no groups, no packed-key table and no
// best-index ordering. A policy of a handful of rules is cheaper to scan than to
// index (the measured crossover is in docs/dataplane-tables.md), and a caller
// that holds one policy per session wants the smallest structure that is still
// exact.
//
// The predicate is shared, not copied: a lookup packs the key to the rule's own
// prefix lengths and calls FirewallTable::Matches, which is what the index does
// per group. A scan therefore pays one packing per rule rather than one per
// group - which is why it is for policies of a few rules, and why the two
// structures cannot disagree about what a rule means.
class FirewallScan {
 public:
  // The same refusals FirewallTable::Build makes, rule for rule: a policy the
  // index accepts is a policy the scan accepts, and the other way round.
  static std::expected<FirewallScan, std::string> Create(
      std::vector<FirewallRule> rules);

  // Classifies `n` (<= 64) keys exactly as FirewallTable::LookupBatch does: bit
  // i set for each key a rule matched, that rule's action in matches[i].action,
  // and its 1-based id when `with_rule_id`. A key nothing matches is left
  // untouched.
  uint64_t LookupBatch(const FirewallKey *keys, size_t n,
                       FirewallTable::Match *matches,
                       bool with_rule_id = true) const noexcept;

  // One key; true when a rule matched.
  bool Lookup(const FirewallKey &key,
              FirewallTable::Match *match) const noexcept;

  // False when no rule can judge a packet of `family` - the same derived answer
  // FirewallTable::HasFamily gives, so a caller can drop stray traffic without
  // a scan.
  bool HasFamily(FirewallFamily family) const noexcept;

  size_t rule_count() const noexcept { return rules_.size(); }
  const std::vector<FirewallRule> &rules() const noexcept { return rules_; }

  // Memory (D-198), all glibc: the rules, taken over from `rules` with
  // their capacity (exact when `rules` is moved in, an upper bound when it
  // is copied).
  static size_t Bytes(const std::vector<FirewallRule> &rules) {
    return rules.capacity() * sizeof(FirewallRule);
  }
  static size_t GlibcBytes(const std::vector<FirewallRule> &rules) {
    return Bytes(rules);
  }
  size_t bytes() const noexcept {
    return rules_.capacity() * sizeof(FirewallRule);
  }

 private:
  FirewallScan() = default;

  std::vector<FirewallRule> rules_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_FIREWALL_TABLE_H_
