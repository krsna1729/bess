// SPDX-License-Identifier: BSD-3-Clause

// Firewall's packet path: the key of a frame and the batch loop. It is a
// translation unit of its own, apart from the control side (firewall_rules.cc):
// GCC's per-unit inline budget (--param inline-unit-growth) is shared by
// everything a unit instantiates, and when the rules resource's templates
// shared this unit, the batch loop lost Task::AllocPacketBatch's inlining.

#include "firewall.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "packet_parse.h"
#include "utils/endian.h"

namespace {

using bess::classifier::FirewallFamily;

// KeyOf's path for every frame but the common one: the checked parse
// ConnTrack, NAT and Router read (D-188), the headers in the first segment,
// their lengths checked against the whole packet. Out of line, so the common
// frame's path stays small enough to inline into the batch loop.
[[gnu::noinline]] bool KeyOfParsed(std::span<const uint8_t> frame, size_t total_len,
                                   bess::classifier::FirewallKey *key) {
  using bess::packet::L3Kind;
  using bess::packet::L4Kind;
  using bess::packet::ParseStatus;
  bess::packet::ParsedFrame p;
  const ParseStatus status = bess::packet::ParseFrame(frame, p, total_len);
  if (status != ParseStatus::kOk && status != ParseStatus::kFragment) {
    return false;  // not IP, or malformed: nothing to judge
  }
  key->family = p.l3 == L3Kind::kIpv6 ? FirewallFamily::kIpv6
                                      : FirewallFamily::kIpv4;
  key->proto = p.protocol;
  key->src = p.src;
  key->dst = p.dst;
  const bool ports = p.l4 == L4Kind::kTcp || p.l4 == L4Kind::kUdp;
  key->src_port = ports ? p.src_port : 0;
  key->dst_port = ports ? p.dst_port : 0;
  return true;
}

}  // namespace

bool Firewall::KeyOf(std::span<const uint8_t> frame, size_t total_len,
                     bess::classifier::FirewallKey *key) {
  using bess::packet::ParseStatus;

  // A fragment other than the first carries no transport header, so its
  // ports are unknown, but the IP header (IPv6: the fragment header) still
  // names the transport protocol: it is judged with ports 0, which only a
  // rule whose port range starts at 0 can match. Reading ports anyway would
  // judge it by whatever bytes follow. Only TCP and UDP have ports to match:
  // the parse's ICMP echo identifier is not one.

  // The common frame: the parse's checks, then the fields at their offsets.
  const bess::packet::FastIpv4 f = bess::packet::CheckFastIpv4(frame, total_len);
  if (!f.taken) [[unlikely]] {
    return KeyOfParsed(frame, total_len, key);
  }
  if (f.status != ParseStatus::kOk && f.status != ParseStatus::kFragment) {
    return false;
  }
  key->family = FirewallFamily::kIpv4;
  key->proto = f.protocol;
  key->src = {};
  key->dst = {};
  std::memcpy(key->src.data(), frame.data() + bess::packet::kFastIpv4Src, 4);
  std::memcpy(key->dst.data(), frame.data() + bess::packet::kFastIpv4Dst, 4);
  const bool ports =
      f.status == ParseStatus::kOk && (f.protocol == 6 || f.protocol == 17);
  const uint8_t *l4 = frame.data() + bess::packet::kFastIpv4L4;
  key->src_port = ports ? static_cast<uint16_t>(unsigned{l4[0]} << 8u | unsigned{l4[1]}) : 0;
  key->dst_port = ports ? static_cast<uint16_t>(unsigned{l4[2]} << 8u | unsigned{l4[3]}) : 0;
  return true;
}

void Firewall::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::classifier::FirewallFamily;
  using bess::classifier::FirewallKey;
  using bess::classifier::FirewallTable;

  const Published *published = published_.Read();
  const FirewallTable *table =
      published == nullptr ? nullptr : published->table.get();
  // D-141: a packet this policy passes carries the action id its rule names,
  // so a module downstream resolves it; 0 when the rule names none, so a stale
  // id cannot reach a later lookup. The offset is valid only when something
  // reads the attribute; with no reader there is nothing to publish to.
  const bess::metadata::mt_offset_t action_offset = attr_offset(action_id_attr_);
  const bool publish_action = bess::metadata::IsValidOffset(action_offset);

  const size_t n = static_cast<size_t>(batch->cnt());
  std::array<FirewallKey, bess::PacketBatch::kMaxBurst> keys;
  std::array<FirewallTable::Match, bess::PacketBatch::kMaxBurst> matches;
  // A policy that names only one family can drop the other's packets without
  // a lookup (derived at build, so it cannot disagree with the rules).
  const bool ipv4_ok =
      table != nullptr && table->HasFamily(FirewallFamily::kIpv4);
  const bool ipv6_ok =
      table != nullptr && table->HasFamily(FirewallFamily::kIpv6);
  uint64_t parseable = 0;
  for (size_t i = 0; i < n; i++) {
    bess::PacketRef pkt = batch->packet(i);
    if (!KeyOf(std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), pkt.total_len(),
               &keys[i])) {
      keys[i] = FirewallKey{};
      continue;
    }
    const bool served = keys[i].family == FirewallFamily::kIpv6 ? ipv6_ok
                                                                : ipv4_ok;
    if (served) {
      parseable |= uint64_t{1} << i;
    }
  }

  // One table lookup for the whole batch; a packet this table cannot judge is
  // masked out of the result, so a catch-all rule cannot claim it.
  uint64_t matched = 0;
  if (table != nullptr && n > 0) {
    // Rule ids only when the action is published: the write needs the matched
    // rule's ActionId, and rule_id is what names the rule (0 otherwise).
    matched =
        table->LookupBatch(keys.data(), n, matches.data(), publish_action) &
        parseable;
  }

  for (size_t i = 0; i < n; i++) {
    bess::PacketRef pkt = batch->packet(i);
    const bool pass = ((matched >> i) & 1) != 0 && matches[i].action == kPass;
    if (pass) {
      if (publish_action) {
        // rule_id is 1-based; action_ids holds one entry per rule, in order.
        const uint32_t id = published->action_ids[matches[i].rule_id - 1];
        _set_attr_with_offset<bess::utils::be32_t>(action_offset, pkt,
                                                   bess::utils::be32_t(id));
      }
      EmitPacket(ctx, pkt, ctx->current_igate);
    } else {
      DropPacket(ctx, pkt);
    }
  }
}
