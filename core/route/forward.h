// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_FORWARD_H_
#define BESS_ROUTE_FORWARD_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "dataplane/interface_id.h"
#include "packet_parse.h"
#include "route/l3_packet.h"
#include "route/route_domain.h"
#include "route/router.h"
#include "utils/ether.h"
#include "utils/ip.h"

namespace bess::route {

// The destination path for IPv4 and IPv6, one implementation: a caller with
// frames (an appliance) and a graph adapter with packets (the Router module)
// both use these functions, so they cannot disagree about a fragment, a TTL
// or hop-limit boundary, a link-scoped address or which hop address is
// written. The frame is parsed once and its family decides the table.
//
// Policy stays outside. Which domain a packet is in is the caller's (an
// interface, metadata, a port), what an unresolved neighbor means is the
// caller's (drop, queue, punt), and so is everything a verdict becomes: a
// gate, a counter, a decision record. The library has no Module, no gate and
// no metadata, so it stays usable by a caller that has none of them
// (tools/check_includes.py), and a caller that wants a new metadata output
// adds it in its own adapter.

// Why a forwarding attempt did or did not put the frame on a wire. After the
// parse verdicts the checks run in this order: family, link scope, lookup,
// local, egress, neighbor, TTL / hop limit.
enum class ForwardVerdict : uint8_t {
  kForward,             // TTL / hop limit decremented, the hop's addresses written
  kLocalDelivery,       // the destination is this node's own address (D-178)
  kNotIp,               // a valid frame that is neither IPv4 nor IPv6
  kMalformed,           // a header does not fit or its lengths are inconsistent
  kNonInitialFragment,  // no L4 header: a group route would hash by something
                        // other than the datagram's first fragment
  kFamilyDisabled,      // the frame's family is not enabled on the router
  kLinkScope,           // IPv6 link-local or multicast destination, or
                        // link-local source: never routed (RFC 4291 2.5.6)
  kNoRoute,             // no route for this destination in this domain
  kUnresolved,          // the route's next hop has no L2 addresses yet
  kTtlExpired,          // TTL / hop limit 0 or 1: nothing written
  kNoEgress,            // the hop names no usable egress interface: nothing written
};

enum class L3Family : uint8_t { kIpv4, kIpv6 };

// What the frame routes to. ResolveFrame() reads the frame and writes nothing,
// so a caller can count, gate or drop without touching bytes it discards.
struct FrameRoute {
  ForwardVerdict verdict = ForwardVerdict::kNotIp;
  // The frame's family, set whenever it parsed as IP.
  L3Family family = L3Family::kIpv4;
  // The hop to write, for kForward.
  const NextHop *hop = nullptr;
  // The hop's egress once one is known: set for kForward, kUnresolved and
  // kTtlExpired, so a caller can say where the frame was headed.
  dataplane::InterfaceId egress = dataplane::kInvalidInterfaceId;
  // Where the IP header starts in the frame, for WriteForwarded(): set for
  // kForward and kTtlExpired.
  uint16_t l3_offset = 0;
};

namespace forward_internal {

// The parse verdicts, and the family for an IP frame. Returns true when the
// frame has a destination to resolve (`out` then holds its family only).
inline bool ParseVerdict(packet::ParseStatus parsed,
                         const packet::ParsedFrame &p,
                         FrameRoute &out) noexcept {
  out = {};
  if (p.l3 == packet::L3Kind::kIpv6) {
    out.family = L3Family::kIpv6;
  }
  switch (parsed) {
    case packet::ParseStatus::kOk:
      break;
    case packet::ParseStatus::kNotIp:
      return false;
    case packet::ParseStatus::kFragment:
      out.verdict = ForwardVerdict::kNonInitialFragment;
      return false;
    default:
      out.verdict = ForwardVerdict::kMalformed;
      return false;
  }
  if (p.l3 != packet::L3Kind::kIpv4 && p.l3 != packet::L3Kind::kIpv6) {
    return false;  // kNotIp
  }
  return true;
}

// The family and link-scope checks: true when the frame goes on to a lookup.
inline bool Routable(const Router &router, const packet::ParsedFrame &p,
                     FrameRoute &out) noexcept {
  if (out.family == L3Family::kIpv4 ? !router.has_ipv4() : !router.has_ipv6()) {
    out.verdict = ForwardVerdict::kFamilyDisabled;
    return false;
  }
  if (out.family == L3Family::kIpv6) {
    utils::Ipv6Address src;
    utils::Ipv6Address dst;
    std::memcpy(src.bytes, p.src.data(), sizeof(src.bytes));
    std::memcpy(dst.bytes, p.dst.data(), sizeof(dst.bytes));
    // A router never forwards a link-local source or destination (RFC 4291
    // 2.5.6), and multicast is not unicast routing's: solicitations, MLD and
    // link-local traffic are the node's own business.
    if (dst.IsLinkLocal() || dst.IsMulticast() || src.IsLinkLocal()) {
      out.verdict = ForwardVerdict::kLinkScope;
      return false;
    }
  }
  return true;
}

// The flow hash a group route picks a member with: the address words are
// opaque, so both callers pass the header's bytes as they are; IPv6 folds its
// four words of each address.
inline uint32_t FrameHash(const packet::ParsedFrame &p, L3Family family,
                          FlowHashFields fields) noexcept {
  uint32_t src_words[4];
  uint32_t dst_words[4];
  std::memcpy(src_words, p.src.data(), sizeof(src_words));
  std::memcpy(dst_words, p.dst.data(), sizeof(dst_words));
  if (family == L3Family::kIpv4) {
    return FlowHash(src_words[0], dst_words[0], p.src_port, p.dst_port,
                    p.protocol, fields);
  }
  return FlowHash(src_words[0] ^ src_words[1] ^ src_words[2] ^ src_words[3],
                  dst_words[0] ^ dst_words[1] ^ dst_words[2] ^ dst_words[3],
                  p.src_port, p.dst_port, p.protocol, fields);
}

// The checks after the lookup: local, egress, neighbor, TTL / hop limit.
// `out` holds the family; `l3_offset` is where the parse found the header.
inline void Settle(const NextHop *hop, std::span<const uint8_t> frame,
                   uint16_t l3_offset, FrameRoute &out) noexcept {
  if (hop == nullptr) {
    out.verdict = ForwardVerdict::kNoRoute;
    return;
  }
  // A local route is delivery, not forwarding (D-178): it has no egress and no
  // neighbor, so it is settled before either is consulted.
  if (hop->kind == NextHopKind::kLocal) {
    out.verdict = ForwardVerdict::kLocalDelivery;
    return;
  }
  // The egress first: a hop that forwards nowhere is the blackhole shape
  // (D-160) whatever its neighbor state says, and router.h promises that state
  // is irrelevant for such a hop -- a route to it ends the packet rather than
  // resolving anything.
  if (hop->egress == dataplane::kInvalidInterfaceId) {
    out.verdict = ForwardVerdict::kNoEgress;
    return;
  }
  out.egress = hop->egress;
  if (hop->neighbor != NeighborState::kResolved) {
    out.verdict = ForwardVerdict::kUnresolved;
    return;
  }
  // The boundary DecrementTtl()/DecrementHopLimit() refuse at, read here so a
  // caller can drop the frame without making it writable (and so without
  // copying a shared payload) first. The parse found the whole header in
  // `frame`.
  out.l3_offset = l3_offset;
  const uint8_t ttl =
      out.family == L3Family::kIpv4
          ? reinterpret_cast<const utils::Ipv4 *>(frame.data() + l3_offset)->ttl
          : frame[l3_offset + 7];
  if (ttl <= 1) {
    out.verdict = ForwardVerdict::kTtlExpired;
    return;
  }
  out.verdict = ForwardVerdict::kForward;
  out.hop = hop;
}

}  // namespace forward_internal

// The read half: the checked parse, the family check, the IPv6 link-scope
// check, the destination resolved in `domain` (a group route picks its member
// with the frame's own flow hash, the one hash both callers use, reading the
// fields the caller names -- D-163), the local, egress and neighbor checks,
// and the TTL / hop limit read before anything is written. `frame` is the
// packet's first segment and `total_len` the whole frame (0: the span's
// length).
inline FrameRoute ResolveFrame(const Router &router, RouteDomainId domain,
                               std::span<const uint8_t> frame, size_t total_len,
                               FlowHashFields fields = FlowHashFields::kL3L4) noexcept {
  packet::ParsedFrame p;
  const packet::ParseStatus parsed = packet::ParseFrame(frame, p, total_len);
  FrameRoute out;
  if (!forward_internal::ParseVerdict(parsed, p, out) ||
      !forward_internal::Routable(router, p, out)) {
    return out;
  }
  const uint32_t hash = forward_internal::FrameHash(p, out.family, fields);
  const NextHop *hop;
  if (out.family == L3Family::kIpv4) {
    // The destination in the LPM's own order (host order, whatever the host
    // is: the endian wrapper converts).
    utils::be32_t dst;
    std::memcpy(&dst, p.dst.data(), sizeof(dst));
    hop = router.Resolve(domain, dst.value(), hash);
  } else {
    utils::Ipv6Address dst;
    std::memcpy(dst.bytes, p.dst.data(), sizeof(dst.bytes));
    hop = router.Resolve(domain, dst, hash);
  }
  forward_internal::Settle(hop, frame, p.l3_offset, out);
  return out;
}

// The write half: the IPv4 TTL (checksum updated) or the IPv6 hop limit, then
// the hop's addresses when it names both; a hop that names none, or only one,
// is forwarded with the header it arrived with (D-153). `frame` must be
// writable and the IP header must lie inside it: a packet whose payload is
// shared, or whose header sits in another segment, is the caller's to make
// contiguous first, because packet ownership is not the library's.
inline TtlResult WriteForwarded(std::span<uint8_t> frame, L3Family family,
                                uint16_t l3_offset, const NextHop &hop) noexcept {
  const TtlResult ttl =
      family == L3Family::kIpv4
          ? DecrementTtl(*reinterpret_cast<utils::Ipv4 *>(frame.data() + l3_offset))
          : DecrementHopLimit(frame.subspan(l3_offset));
  if (ttl != TtlResult::kForward) {
    return ttl;
  }
  if (!hop.dst_mac.IsZero() && !hop.src_mac.IsZero()) {
    auto *eth = reinterpret_cast<utils::Ethernet *>(frame.data());
    eth->dst_addr = hop.dst_mac;
    eth->src_addr = hop.src_mac;
  }
  return TtlResult::kForward;
}

// The batch read half: ResolveFrame's stages for frames that share one domain,
// with the lookups done once per family for the whole batch -- one IPv4 bulk
// lookup for the IPv4 frames (one rte_fib_lookup_bulk with that backend, the
// gap live_perf's M.router/fib row shows, D-132, K7) and one
// Router::ResolveBatch6 (one rte_fib6_lookup_bulk) for the IPv6 frames.
//
// The parse and the checks are frame-local and run per frame; only the
// lookups are shared. A frame the parse, family or link-scope checks settle is
// settled without a lookup.
//
// Every frame must be in `domain`: each lookup is one call for one table. A
// caller whose frames carry different domains groups them, and can fall back
// to ResolveFrame.
//
// `frames[i]` is the packet's first segment and `total_lens[i]` the whole frame
// (0: the span's length). At most Router::kMaxBatch frames.
inline void ResolveFrames(const Router &router, RouteDomainId domain,
                          std::span<const std::span<const uint8_t>> frames,
                          std::span<const size_t> total_lens,
                          std::span<FrameRoute> out,
                          FlowHashFields fields = FlowHashFields::kL3L4) noexcept {
  promise(frames.size() == out.size() && frames.size() == total_lens.size());
  promise(frames.size() <= Router::kMaxBatch);
  // Per family: the destinations, the hashes, where each looked-up frame came
  // from and its header offset.
  uint32_t dsts4[Router::kMaxBatch];
  // Ipv6Address zero-initialises its bytes; a union leaves the array raw, and
  // only the n6 elements a lookup reads are constructed below.
  union Ipv6Batch {
    Ipv6Batch() {}
    utils::Ipv6Address at[Router::kMaxBatch];
  } dsts6;
  uint32_t hashes4[Router::kMaxBatch];
  uint32_t hashes6[Router::kMaxBatch];
  uint8_t positions4[Router::kMaxBatch];
  uint8_t positions6[Router::kMaxBatch];
  uint16_t offsets4[Router::kMaxBatch];
  uint16_t offsets6[Router::kMaxBatch];
  // Not zeroed: the mask is the authority, and a batch resolve clears the bit
  // for a position it leaves unwritten, so only written elements are read.
  const NextHop *hops4[Router::kMaxBatch];
  const NextHop *hops6[Router::kMaxBatch];
  size_t n4 = 0;
  size_t n6 = 0;
  for (size_t i = 0; i < frames.size(); i++) {
    packet::ParsedFrame p;
    const packet::ParseStatus parsed =
        packet::ParseFrame(frames[i], p, total_lens[i]);
    if (!forward_internal::ParseVerdict(parsed, p, out[i]) ||
        !forward_internal::Routable(router, p, out[i])) {
      continue;
    }
    const uint32_t hash = forward_internal::FrameHash(p, out[i].family, fields);
    if (out[i].family == L3Family::kIpv4) {
      utils::be32_t dst;
      std::memcpy(&dst, p.dst.data(), sizeof(dst));
      dsts4[n4] = dst.value();
      hashes4[n4] = hash;
      positions4[n4] = static_cast<uint8_t>(i);
      offsets4[n4] = p.l3_offset;
      n4++;
    } else {
      std::memcpy(std::construct_at(&dsts6.at[n6])->bytes, p.dst.data(),
                  sizeof(utils::Ipv6Address::bytes));
      hashes6[n6] = hash;
      positions6[n6] = static_cast<uint8_t>(i);
      offsets6[n6] = p.l3_offset;
      n6++;
    }
  }
  if (n4 != 0) {
    const uint64_t mask =
        router.ResolveBatch(domain, std::span(dsts4, n4),
                            std::span(hashes4, n4), std::span(hops4, n4));
    for (size_t b = 0; b < n4; b++) {
      const size_t i = positions4[b];
      forward_internal::Settle((mask >> b & 1) != 0 ? hops4[b] : nullptr,
                               frames[i], offsets4[b], out[i]);
    }
  }
  if (n6 != 0) {
    const uint64_t mask =
        router.ResolveBatch6(domain, std::span(dsts6.at, n6),
                             std::span(hashes6, n6), std::span(hops6, n6));
    for (size_t b = 0; b < n6; b++) {
      const size_t i = positions6[b];
      forward_internal::Settle((mask >> b & 1) != 0 ? hops6[b] : nullptr,
                               frames[i], offsets6[b], out[i]);
    }
  }
}

}  // namespace bess::route

#endif  // BESS_ROUTE_FORWARD_H_
