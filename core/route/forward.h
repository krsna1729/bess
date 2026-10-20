// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_FORWARD_H_
#define BESS_ROUTE_FORWARD_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "conntrack/packet_parse.h"
#include "dataplane/interface_id.h"
#include "route/l3_packet.h"
#include "route/route_domain.h"
#include "route/router.h"
#include "utils/ether.h"
#include "utils/ip.h"

namespace bess::route {

// The IPv4 destination path, one implementation: a caller with frames (an
// appliance) and a graph adapter with packets (the Router module) both use
// these two functions, so they cannot disagree about a fragment, a TTL
// boundary or which hop address is written.
//
// Policy stays outside. Which domain a packet is in is the caller's (an
// interface, metadata, a port), what an unresolved neighbor means is the
// caller's (drop, queue, punt), and so is everything a verdict becomes: a
// gate, a counter, a decision record. The library has no Module, no gate and
// no metadata, so it stays usable by a caller that has none of them
// (tools/check_includes.py), and a caller that wants a new metadata output
// adds it in its own adapter.

// Why a forwarding attempt did or did not put the frame on a wire.
enum class ForwardVerdict : uint8_t {
  kForward,             // TTL decremented, the hop's addresses written
  kNotIpv4,             // a valid frame that is not an IPv4 packet
  kMalformed,           // a header does not fit or its lengths are inconsistent
  kNonInitialFragment,  // no L4 header: a group route would hash by something
                        // other than the datagram's first fragment
  kNoRoute,             // no route for this destination in this domain
  kUnresolved,          // the route's next hop has no L2 addresses yet
  kTtlExpired,          // TTL 0 or 1: nothing written
  kNoEgress,            // the hop names no usable egress interface: nothing written
};

// What the frame routes to. ResolveFrame() reads the frame and writes nothing,
// so a caller can count, gate or drop without touching bytes it discards.
struct FrameRoute {
  ForwardVerdict verdict = ForwardVerdict::kNotIpv4;
  // The hop to write, for kForward.
  const NextHop *hop = nullptr;
  // The hop's egress once one is known: set for kForward, kUnresolved and
  // kTtlExpired, so a caller can say where the frame was headed.
  dataplane::InterfaceId egress = dataplane::kInvalidInterfaceId;
  // Where the IPv4 header starts in the frame, for WriteForwarded().
  uint16_t l3_offset = 0;
};

// The read half: the checked parse, the destination resolved in `domain` (a
// group route picks its member with the frame's own flow hash, the one hash
// both callers use, reading the fields the caller names -- D-163), the egress
// check, the neighbor check, and the TTL read before anything is written.
// `frame` is the packet's first segment and `total_len` the whole frame (0: the
// span's length).
inline FrameRoute ResolveFrame(const Router &router, RouteDomainId domain,
                               std::span<const uint8_t> frame, size_t total_len,
                               FlowHashFields fields = FlowHashFields::kL3L4) noexcept {
  conntrack::ParsedFlowPacket p;
  const conntrack::ParseStatus parsed = conntrack::ParseFrame(frame, p, total_len);
  if (parsed == conntrack::ParseStatus::kNotIp) {
    return {};
  }
  if (parsed == conntrack::ParseStatus::kFragment) {
    return {.verdict = ForwardVerdict::kNonInitialFragment};
  }
  if (parsed != conntrack::ParseStatus::kOk) {
    return {.verdict = ForwardVerdict::kMalformed};
  }
  if (p.l3 != conntrack::L3Kind::kIpv4) {
    return {};
  }
  // The destination in the LPM's own order (host order, whatever the host is:
  // the endian wrapper converts) and the flow hash a group route picks a member
  // with. The address words are opaque, so both callers pass the header's bytes
  // as they are.
  utils::be32_t dst;
  std::memcpy(&dst, p.dst.data(), sizeof(dst));
  uint32_t src_word, dst_word;
  std::memcpy(&src_word, p.src.data(), sizeof(src_word));
  std::memcpy(&dst_word, p.dst.data(), sizeof(dst_word));
  const NextHop *hop = router.Resolve(
      domain, dst.value(),
      FlowHash(src_word, dst_word, p.src_port, p.dst_port, p.protocol, fields));
  if (hop == nullptr) {
    return {.verdict = ForwardVerdict::kNoRoute};
  }
  // The egress first: a hop that forwards nowhere is the blackhole shape
  // (D-160) whatever its neighbor state says, and router.h promises that state
  // is irrelevant for such a hop -- a route to it ends the packet rather than
  // resolving anything. The other order reported a blackhole hop's frames as
  // kUnresolved, which the caller counts as its own outcome.
  if (hop->egress == dataplane::kInvalidInterfaceId) {
    return {.verdict = ForwardVerdict::kNoEgress};
  }
  if (hop->neighbor != NeighborState::kResolved) {
    return {.verdict = ForwardVerdict::kUnresolved, .egress = hop->egress};
  }
  const auto *ip = reinterpret_cast<const utils::Ipv4 *>(frame.data() + p.l3_offset);
  if (ip->ttl <= 1) {
    // The boundary DecrementTtl() refuses at, read here so a caller can drop
    // the frame without making it writable (and so without copying a shared
    // payload) first.
    return {.verdict = ForwardVerdict::kTtlExpired,
            .egress = hop->egress,
            .l3_offset = p.l3_offset};
  }
  return {.verdict = ForwardVerdict::kForward,
          .hop = hop,
          .egress = hop->egress,
          .l3_offset = p.l3_offset};
}

// The write half: the TTL, then the hop's addresses when it names both; a hop
// that names none, or only one, is forwarded with the header it arrived with
// (D-153). `frame` must be writable and the IPv4 header must lie inside it: a
// packet whose payload is shared, or whose header sits in another segment, is
// the caller's to make contiguous first, because packet ownership is not the
// library's.
inline TtlResult WriteForwarded(std::span<uint8_t> frame, uint16_t l3_offset,
                                const NextHop &hop) noexcept {
  auto *ip = reinterpret_cast<utils::Ipv4 *>(frame.data() + l3_offset);
  const TtlResult ttl = DecrementTtl(*ip);
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
// with the lookup done once for the whole batch, so a backend that has a bulk
// lookup uses it -- one rte_fib_lookup_bulk per batch instead of one per frame,
// which is the gap live_perf's M.router/fib row shows (D-132, K7).
//
// The parse and the checks are frame-local and run per frame; only the lookup
// is shared. A frame the parse settles (not IP, a fragment, malformed) is
// settled without a lookup, so the batch the lookup sees holds only the frames
// that have a destination to resolve.
//
// Every frame must be in `domain`: the lookup is one call for one table, which
// is what Router::ResolveBatch takes. A caller whose frames carry different
// domains groups them, and can fall back to ResolveFrame.
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
  uint32_t dsts[Router::kMaxBatch];
  uint32_t hashes[Router::kMaxBatch];
  size_t positions[Router::kMaxBatch];  // where each looked-up frame came from
  uint16_t offsets[Router::kMaxBatch];  // its IPv4 header, for the TTL read
  // Not zeroed: the mask is the authority, and ResolveBatchIn clears the bit
  // for a position it leaves unwritten, so only written elements are read.
  const NextHop *hops[Router::kMaxBatch];
  size_t batch = 0;
  for (size_t i = 0; i < frames.size(); i++) {
    conntrack::ParsedFlowPacket p;
    const conntrack::ParseStatus parsed = conntrack::ParseFrame(frames[i], p, total_lens[i]);
    if (parsed == conntrack::ParseStatus::kNotIp) {
      out[i] = {};
      continue;
    }
    if (parsed == conntrack::ParseStatus::kFragment) {
      out[i] = {.verdict = ForwardVerdict::kNonInitialFragment};
      continue;
    }
    if (parsed != conntrack::ParseStatus::kOk) {
      out[i] = {.verdict = ForwardVerdict::kMalformed};
      continue;
    }
    if (p.l3 != conntrack::L3Kind::kIpv4) {
      out[i] = {};
      continue;
    }
    // The destination in the LPM's own order and the hash a group route picks
    // a member with, as ResolveFrame reads them.
    utils::be32_t dst;
    std::memcpy(&dst, p.dst.data(), sizeof(dst));
    uint32_t src_word, dst_word;
    std::memcpy(&src_word, p.src.data(), sizeof(src_word));
    std::memcpy(&dst_word, p.dst.data(), sizeof(dst_word));
    dsts[batch] = dst.value();
    hashes[batch] = FlowHash(src_word, dst_word, p.src_port, p.dst_port, p.protocol, fields);
    positions[batch] = i;
    offsets[batch] = p.l3_offset;
    out[i] = {};
    batch++;
  }
  const uint64_t mask =
      batch == 0 ? 0
                 : router.ResolveBatch(domain, std::span(dsts, batch),
                                       std::span(hashes, batch), std::span(hops, batch));
  for (size_t b = 0; b < batch; b++) {
    const size_t i = positions[b];
    const NextHop *hop = (mask & (uint64_t{1} << b)) != 0 ? hops[b] : nullptr;
    if (hop == nullptr) {
      out[i].verdict = ForwardVerdict::kNoRoute;
      continue;
    }
    // The egress first, as ResolveFrame has it: a hop that forwards nowhere is
    // the blackhole shape (D-160) whatever its neighbor state says.
    if (hop->egress == dataplane::kInvalidInterfaceId) {
      out[i].verdict = ForwardVerdict::kNoEgress;
      continue;
    }
    if (hop->neighbor != NeighborState::kResolved) {
      out[i].verdict = ForwardVerdict::kUnresolved;
      out[i].egress = hop->egress;
      continue;
    }
    // The header offset is set where ResolveFrame sets it -- with the forward
    // and the TTL boundary, the two verdicts a caller writes bytes for.
    const auto *ip = reinterpret_cast<const utils::Ipv4 *>(frames[i].data() + offsets[b]);
    if (ip->ttl <= 1) {
      out[i].verdict = ForwardVerdict::kTtlExpired;
      out[i].egress = hop->egress;
      out[i].l3_offset = offsets[b];
      continue;
    }
    out[i].verdict = ForwardVerdict::kForward;
    out[i].hop = hop;
    out[i].egress = hop->egress;
    out[i].l3_offset = offsets[b];
  }
}

}  // namespace bess::route

#endif  // BESS_ROUTE_FORWARD_H_
