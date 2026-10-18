// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/firewall_table.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>

#include "arch/masked_addr.h"
#include "utils/common.h"

namespace bess::classifier {

namespace {
constexpr uint8_t kAnyProto = 0;
constexpr uint32_t kNoRule = 0;

// The family's address width in bytes: 4, 16, or 0 for "any family" (a rule
// that carries no prefix at all).
size_t FamilyBytes(FirewallFamily family) {
  switch (family) {
    case FirewallFamily::kIpv4:
      return 4;
    case FirewallFamily::kIpv6:
      return 16;
    case FirewallFamily::kAny:
      break;
  }
  return 0;
}

// The width of a family's prefix length: 32, 128, or 0.
uint8_t FamilyBits(FirewallFamily family) {
  return static_cast<uint8_t>(FamilyBytes(family) * 8);
}

uint32_t Load32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint64_t Load64(const uint8_t *p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Two 16-byte values equal, as two 64-bit compares rather than a memcmp call.
bool Bytes16Equal(const uint8_t *a, const uint8_t *b) {
  return Load64(a) == Load64(b) && Load64(a + 8) == Load64(b + 8);
}

// The IPv6 packed key as four 64-bit words. The order is this comparison's,
// not the bytes': all that matters is that the sort and the search use the
// same one, and that equality means every byte is equal - which a memcmp over
// 32 bytes spends four times the work to say.
struct Key64 {
  uint64_t w[4];

  static Key64 Load(const uint8_t *p) {
    return Key64{{Load64(p), Load64(p + 8), Load64(p + 16), Load64(p + 24)}};
  }
  bool operator==(const Key64 &other) const {
    return w[0] == other.w[0] && w[1] == other.w[1] && w[2] == other.w[2] &&
           w[3] == other.w[3];
  }
  bool operator<(const Key64 &other) const {
    if (w[0] != other.w[0]) {
      return w[0] < other.w[0];
    }
    if (w[1] != other.w[1]) {
      return w[1] < other.w[1];
    }
    if (w[2] != other.w[2]) {
      return w[2] < other.w[2];
    }
    return w[3] < other.w[3];
  }
};

// The IPv4 pair of masks as one 64-bit value in the packed layout: source in
// the low half, destination in the high half. `a & mask` on the loaded pair is
// then the whole masking step for an IPv4 key.
uint64_t Mask64(const FirewallAddr &src, const FirewallAddr &dst) {
  return Load32(src.data()) | (static_cast<uint64_t>(Load32(dst.data())) << 32);
}

// The byte mask of `len` bits: 0xff for each whole byte, a partial mask for
// the last one it covers, zero after. Built once per rule at Build, never in
// the packet path.
FirewallAddr MaskBytes(uint8_t len, FirewallFamily family) {
  FirewallAddr out{};
  const size_t bytes = FamilyBytes(family);
  for (size_t i = 0; i < bytes; i++) {
    const size_t bits_before = i * 8;
    if (len >= bits_before + 8) {
      out[i] = 0xff;
    } else if (len > bits_before) {
      out[i] = static_cast<uint8_t>(0xffu << (8 - (len - bits_before)));
    } else {
      break;  // the rest is already zero
    }
  }
  return out;
}
}  // namespace

FirewallAddr FirewallTable::MaskedAddr(const FirewallAddr &addr, uint8_t len,
                                       FirewallFamily family) noexcept {
  // The build path, once per rule: a loop is right here. The packet path
  // masks a whole pair with the arch kernel (arch/masked_addr.h).
  const FirewallAddr mask = MaskBytes(len, family);
  FirewallAddr out{};
  const size_t bytes = FamilyBytes(family);
  for (size_t i = 0; i < bytes; i++) {
    out[i] = static_cast<uint8_t>(addr[i] & mask[i]);
  }
  return out;
}

FirewallTable::PackedKey FirewallTable::Pack(const FirewallAddr &src,
                                             uint8_t src_len,
                                             const FirewallAddr &dst,
                                             uint8_t dst_len,
                                             FirewallFamily family) noexcept {
  PackedKey key{};
  const FirewallAddr mask_src = MaskBytes(src_len, family);
  const FirewallAddr mask_dst = MaskBytes(dst_len, family);
  // Compact per family: source then destination with no gap, so the bytes a
  // group compares (`key_bytes`) are exactly the ones that carry meaning - 8
  // for IPv4, 32 for IPv6, none for a group with no prefix.
  if (family == FirewallFamily::kIpv6) {
    bess::arch::MaskedAddrPair16x2(key.data(), src.data(), dst.data(),
                                   mask_src.data(), mask_dst.data());
    return key;
  }
  const size_t bytes = FamilyBytes(family);
  for (size_t i = 0; i < bytes; i++) {
    key[i] = static_cast<uint8_t>(src[i] & mask_src[i]);
    key[bytes + i] = static_cast<uint8_t>(dst[i] & mask_dst[i]);
  }
  return key;
}

bool FirewallTable::Matches(const FirewallRule &rule, const FirewallKey &key,
                            const PackedKey &packed) noexcept {
  // The group already settled the family, and it masked the addresses to the
  // rule's own prefix lengths - so a prefix is a compare against the packed
  // key's halves, not another mask.
  if (rule.proto != kAnyProto && rule.proto != key.proto) {
    return false;
  }
  if (rule.family == FirewallFamily::kIpv6) {
    if (rule.src_len != 0 &&
        !Bytes16Equal(rule.src_net.data(), packed.data())) {
      return false;
    }
    if (rule.dst_len != 0 &&
        !Bytes16Equal(rule.dst_net.data(), packed.data() + 16)) {
      return false;
    }
  } else {
    if (rule.src_len != 0 &&
        Load32(rule.src_net.data()) != Load32(packed.data())) {
      return false;
    }
    if (rule.dst_len != 0 &&
        Load32(rule.dst_net.data()) != Load32(packed.data() + 4)) {
      return false;
    }
  }
  // Inclusive ranges: two comparisons each, and no mask anywhere - this is
  // what a WildcardMatch rule cannot do without spending its tuple budget
  // (D-102). A rule with no port constraint skips all four: Build derived the
  // flag.
  if (rule.checks_ports) {
    if (key.src_port < rule.src_port_low || key.src_port > rule.src_port_high) {
      return false;
    }
    if (key.dst_port < rule.dst_port_low || key.dst_port > rule.dst_port_high) {
      return false;
    }
  }
  return true;
}

// The refusals both structures make, rule for rule, so a policy the index
// accepts is a policy the scan accepts and the other way round. `index` is
// 1-based, the way the messages count rules and the way a Match's rule_id does.
std::optional<std::string> CheckRule(const FirewallRule &rule,
                                     uint32_t index) {
  const std::string where = "rule " + std::to_string(index) + ": ";
  switch (rule.family) {
    case FirewallFamily::kIpv4:
    case FirewallFamily::kIpv6:
      break;
    case FirewallFamily::kAny:
      // A rule with no family cannot carry a prefix: there would be no width
      // to read the length against.
      if (rule.src_len != 0 || rule.dst_len != 0) {
        return where +
               "a rule that names no family cannot carry a prefix";
      }
      break;
  }
  const uint8_t bits = FamilyBits(rule.family);
  if (rule.src_len > bits) {
    return where + "source prefix length " + std::to_string(rule.src_len) +
           " is beyond " + std::to_string(bits);
  }
  if (rule.dst_len > bits) {
    return where + "destination prefix length " + std::to_string(rule.dst_len) +
           " is beyond " + std::to_string(bits);
  }
  if (rule.src_port_low > rule.src_port_high) {
    return where + "source port range " + std::to_string(rule.src_port_low) +
           "-" + std::to_string(rule.src_port_high) +
           " has its low bound above its high";
  }
  if (rule.dst_port_low > rule.dst_port_high) {
    return where + "destination port range " +
           std::to_string(rule.dst_port_low) + "-" +
           std::to_string(rule.dst_port_high) +
           " has its low bound above its high";
  }
  return std::nullopt;
}

// What both structures do to a rule once it is known to be expressible: mask
// the addresses to their prefix lengths - a sloppy prefix means its network,
// the same reading the UPF gives an SDF prefix - and derive whether the ports
// need checking at all, a full 0..65535 range being no check. The predicate
// reads both, so a structure that skips this answers differently: that is not a
// hypothetical, it is what the scan did before this function existed.
void NormalizeRule(FirewallRule &rule) {
  rule.src_net =
      FirewallTable::MaskedAddr(rule.src_net, rule.src_len, rule.family);
  rule.dst_net =
      FirewallTable::MaskedAddr(rule.dst_net, rule.dst_len, rule.family);
  rule.checks_ports = rule.src_port_low != 0 || rule.src_port_high != 0xffff ||
                      rule.dst_port_low != 0 || rule.dst_port_high != 0xffff;
}

std::expected<std::unique_ptr<const FirewallTable>, std::string>
FirewallTable::Build(std::vector<FirewallRule> rules) {
  auto table = std::unique_ptr<FirewallTable>(new FirewallTable());
  table->rules_.reserve(rules.size());

  // Group identity: family, protocol, then the two prefix lengths. The map's
  // order is irrelevant - the groups are sorted by first_rule below.
  using GroupKey = std::tuple<uint8_t, uint8_t, uint8_t, uint8_t>;
  std::map<GroupKey, Group> by_key;

  uint32_t index = 0;  // 0 is "no rule", so rules are 1-based
  for (FirewallRule &rule : rules) {
    index++;
    if (std::optional<std::string> refused = CheckRule(rule, index);
        refused.has_value()) {
      return std::unexpected(*refused);
    }

    const GroupKey group_key{static_cast<uint8_t>(rule.family), rule.proto,
                             rule.src_len, rule.dst_len};
    Group &group = by_key[group_key];
    group.family = rule.family;
    group.proto = rule.proto;
    group.src_len = rule.src_len;
    group.dst_len = rule.dst_len;
    // Two addresses, but only as many bytes as the family occupies.
    group.key_bytes = static_cast<uint8_t>(2 * FamilyBytes(rule.family));
    group.mask_src = MaskBytes(rule.src_len, rule.family);
    group.mask_dst = MaskBytes(rule.dst_len, rule.family);
    group.mask64 = Mask64(group.mask_src, group.mask_dst);
    // The group's best (lowest) rule index: rules are walked in policy order,
    // so the first one to create the group is its minimum, and the early exit
    // in Lookup() rests on it.
    if (group.entries.empty()) {
      group.first_rule = index;
    }

    // A sloppy prefix means its network (the same reading the UPF gives an SDF
    // prefix), and the bytes beyond the family's width are zero: mask once,
    // here, so the packet path compares a masked network and never the
    // caller's original address.
    NormalizeRule(rule);

    group.entries.push_back(
        Entry{Pack(rule.src_net, rule.src_len, rule.dst_net, rule.dst_len,
                   rule.family),
              index});
    switch (rule.family) {
      case FirewallFamily::kIpv4:
        table->stats_.ipv4_rules++;
        break;
      case FirewallFamily::kIpv6:
        table->stats_.ipv6_rules++;
        break;
      case FirewallFamily::kAny:
        table->stats_.any_family_rules++;
        break;
    }
    table->rules_.push_back(rule);
  }

  table->groups_.reserve(by_key.size());
  for (auto &[key, group] : by_key) {
    (void)key;
    // Sorted by key, in the same order the lookup compares them (a 64-bit
    // compare for IPv4, memcmp for IPv6), and equal keys keep the push order,
    // which is rule order, so the first matching entry of a key is the best
    // rule for that key. The memcmp width is the literal 32 so the compare
    // inlines instead of calling memcmp: the keys of a group that is not IPv4
    // are zero-padded (a group with no prefix holds all-zero keys, which
    // compare equal whatever the width).
    const uint8_t bytes = group.key_bytes;
    std::stable_sort(group.entries.begin(), group.entries.end(),
                     [bytes](const Entry &a, const Entry &b) {
                       if (bytes == 8) {
                         return Load64(a.key.data()) < Load64(b.key.data());
                       }
                       return Key64::Load(a.key.data()) <
                              Key64::Load(b.key.data());
                     });
    table->groups_.push_back(std::move(group));
  }
  // Best rule first: a lookup can stop once a group cannot beat the match in
  // hand.
  std::sort(table->groups_.begin(), table->groups_.end(),
            [](const Group &a, const Group &b) {
              return a.first_rule < b.first_rule;
            });

  table->stats_.rules = table->rules_.size();
  table->stats_.groups = table->groups_.size();
  return table;
}

uint32_t FirewallTable::MatchInGroup(const Group &group,
                                     const FirewallKey &key) const noexcept {
  if (group.family != FirewallFamily::kAny && group.family != key.family) {
    return kNoRule;
  }
  if (group.proto != kAnyProto && group.proto != key.proto) {
    return kNoRule;
  }

  // Mask the packet's addresses with the group's precomputed masks: for IPv4
  // that is two 32-bit loads, a shift, an OR and an AND - the whole key of an
  // IPv4 group is one register.
  PackedKey want{};
  if (group.key_bytes == 8) {
    const uint64_t raw = Load32(key.src.data()) |
                         (static_cast<uint64_t>(Load32(key.dst.data())) << 32);
    const uint64_t masked = raw & group.mask64;
    std::memcpy(want.data(), &masked, sizeof(masked));
  } else if (group.key_bytes == 32) {
    bess::arch::MaskedAddrPair16x2(want.data(), key.src.data(), key.dst.data(),
                                   group.mask_src.data(),
                                   group.mask_dst.data());
  }

  const auto begin = group.entries.begin();
  const auto end = group.entries.end();
  // One binary search, then the entries that share the key in rule order. The
  // two widths are spelled out rather than branched per compare: for IPv4 the
  // key is a 64-bit integer and the compare must not become a memcmp call,
  // and for the rest the width is the literal 32 so it inlines.
  if (group.key_bytes == 8) {
    const uint64_t want64 = Load64(want.data());
    auto it = std::lower_bound(begin, end, want64,
                               [](const Entry &entry, uint64_t value) {
                                 return Load64(entry.key.data()) < value;
                               });
    for (; it != end && Load64(it->key.data()) == want64; ++it) {
      if (Matches(rules_[it->rule - 1], key, want)) {
        return it->rule;
      }
    }
    return kNoRule;
  }
  auto it = std::lower_bound(begin, end, want,
                             [](const Entry &entry, const PackedKey &value) {
                               return Key64::Load(entry.key.data()) <
                                      Key64::Load(value.data());
                             });
  for (; it != end && Key64::Load(it->key.data()) == Key64::Load(want.data());
       ++it) {
    if (Matches(rules_[it->rule - 1], key, want)) {
      return it->rule;
    }
  }
  return kNoRule;
}

bool FirewallTable::Lookup(const FirewallKey &key, Match *match) const noexcept {
  uint32_t best = kNoRule;
  for (const Group &group : groups_) {
    if (best != kNoRule && group.first_rule >= best) {
      // The groups are ordered by their best rule, so no later group can hold
      // a rule better than the match in hand.
      break;
    }
    const uint32_t found = MatchInGroup(group, key);
    if (found != kNoRule && (best == kNoRule || found < best)) {
      best = found;
    }
  }
  if (best == kNoRule) {
    return false;
  }
  match->action = rules_[best - 1].action;
  match->rule_id = best;
  return true;
}

uint64_t FirewallTable::LookupBatch(const FirewallKey *keys, size_t n,
                                    Match *matches, bool with_rule_id) const
    noexcept {
  promise(n <= 64);
  uint64_t matched = 0;
  for (size_t i = 0; i < n; i++) {
    Match found;
    if (!Lookup(keys[i], &found)) {
      continue;
    }
    matched |= uint64_t{1} << i;
    matches[i].action = found.action;
    if (with_rule_id) {
      matches[i].rule_id = found.rule_id;
    }
  }
  return matched;
}

std::expected<FirewallScan, std::string> FirewallScan::Create(
    std::vector<FirewallRule> rules) {
  for (size_t i = 0; i < rules.size(); i++) {
    if (std::optional<std::string> refused =
            CheckRule(rules[i], static_cast<uint32_t>(i + 1));
        refused.has_value()) {
      return std::unexpected(*refused);
    }
  }
  // The same normalization Build applies, through the same function: the
  // shared predicate reads the masked network and the derived port flag, so a
  // scan that stored the caller's rule as given would answer differently - both
  // ways, as it turned out, missing rules it should find and finding rules it
  // should not.
  for (FirewallRule &rule : rules) {
    NormalizeRule(rule);
  }
  FirewallScan scan;
  scan.rules_ = std::move(rules);
  return scan;
}

bool FirewallScan::Lookup(const FirewallKey &key,
                          FirewallTable::Match *match) const noexcept {
  for (size_t i = 0; i < rules_.size(); i++) {
    const FirewallRule &rule = rules_[i];
    // The family is part of a rule's identity in the index (a group is one
    // family); here it is this test, and a rule that names no family sees both.
    if (rule.family != FirewallFamily::kAny && rule.family != key.family) {
      continue;
    }
    // The index packs once per group; a scan packs once per rule, which is what
    // it trades for having no groups at all.
    const FirewallTable::PackedKey packed =
        FirewallTable::Pack(key.src, rule.src_len, key.dst, rule.dst_len,
                            key.family);
    if (FirewallTable::Matches(rule, key, packed)) {
      match->action = rule.action;
      match->rule_id = static_cast<uint32_t>(i + 1);
      return true;
    }
  }
  return false;
}

uint64_t FirewallScan::LookupBatch(const FirewallKey *keys, size_t n,
                                   FirewallTable::Match *matches,
                                   bool with_rule_id) const noexcept {
  uint64_t mask = 0;
  for (size_t i = 0; i < n && i < 64; i++) {
    FirewallTable::Match match;
    if (!Lookup(keys[i], &match)) {
      continue;  // a key nothing matches is left untouched
    }
    mask |= uint64_t{1} << i;
    matches[i].action = match.action;
    if (with_rule_id) {
      matches[i].rule_id = match.rule_id;
    }
  }
  return mask;
}

bool FirewallScan::HasFamily(FirewallFamily family) const noexcept {
  for (const FirewallRule &rule : rules_) {
    if (rule.family == FirewallFamily::kAny || rule.family == family) {
      return true;
    }
  }
  return false;
}

}  // namespace bess::classifier
