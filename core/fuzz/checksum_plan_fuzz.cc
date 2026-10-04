// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes checksum preparation on hand-built mbuf chains: InspectChecksumPlan,
// ComputeChecksums and ApplySoftwareChecksums (packet_checksum.h), and
// BindTxFinalizationProfile + FinalizeTxPacket (packet_tx_checksum.h).
// Apply/Finalize are allocation-free here: every segment is direct with
// refcnt 1, so no copy-on-write path is taken and nothing is freed.
//
// Input (FuzzInput, little-endian; reads past the end are zero):
//   plan    u8 ctl: bits0-1 IpVersion, bits2-3 NetworkChecksum, bits4-5
//           TransportChecksum (raw: out-of-range values are invalid plans),
//           bit6 rewrite the IP/UDP length fields to fit the packet,
//           bit7 derive transport_offset from the IP header
//           u16 network_offset, u16 transport_offset (ignored with bit7)
//   split   u8 count (% 8), count x u8 segment length (the last segment takes
//           the rest), u8 headroom, u8 tailroom
//   profile u8 ctl: bit0 outer, bit1 inner, bit2 outer is its own plan (else
//           the plan above), bits3-5 TxTunnelEncoding (raw), bits6-7 outer
//           IpVersion (raw); u16 capability bits (ipv4_header, udp, tcp,
//           outer_ipv4_header, outer_udp, generic_ip, generic_udp, gtp,
//           multi_segment_tx); u16 outer_network_offset; then a 5-byte plan
//           (ctl as above without bits6-7, u16, u16) for an own outer and one
//           for the inner domain when present
//   packet  rest of the input (at most 2048 bytes)
//   Segment lengths, headroom and network offsets take any value, odd
//   included (checksum.h loads unaligned-safely since M21).
//
// Oracle:
//   - An independent reference parser over the flat bytes decides whether
//     the plan is valid for the packet and gives the layout; Inspect and
//     Compute accept exactly then, the layout matches, and the checksums
//     equal a naive big-endian one's-complement sum (IPv4 header with the
//     field zeroed; pseudo header + transport bytes with the field zeroed;
//     UDP 0 -> 0xffff); only requested values are present.
//   - Results (values and errors) are identical for the one-segment and the
//     split chain, and Inspect and Compute agree on every error.
//   - ApplySoftwareChecksums succeeds iff Compute does; afterwards the chain
//     (descriptors, private area, rooms, data) is byte-identical except the
//     checksum fields, which hold the computed values; recomputing gives the
//     same values; on error nothing changes.
//   - BindTxFinalizationProfile accepts exactly the reference's valid
//     profiles and binds each requested field to hardware iff the
//     capabilities (and tunnel encoding) support it, else software.
//   - FinalizeTxPacket, untunneled: succeeds iff Compute does; afterwards
//     software fields hold the computed checksum, hardware fields hold 0
//     (IPv4 header) or the pseudo-header seed, ol_flags/l2/l3/l4 lengths
//     are exactly the hardware request, and hardware falls back to software
//     exactly when the chain is multi-segment without multi_segment_tx, a
//     header leaves the head segment or l2_len does not fit its bitfield;
//     nothing else in the chain changes. Tunneled: ol_flags never carry an
//     offload the bound profile did not bind to hardware and the topology
//     is unchanged.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <rte_mbuf.h>

#include "fuzz/fuzz_support.h"
#include "packet.h"
#include "packet_checksum.h"
#include "packet_tx_checksum.h"

namespace {

using bess::fuzz::FuzzInput;
using bess::fuzz::MbufChain;
using bess::fuzz::SegmentSpec;
using bess::packet::BindTxFinalizationProfile;
using bess::packet::BoundTxChecksumDomain;
using bess::packet::BoundTxFinalizationProfile;
using bess::packet::ChecksumError;
using bess::packet::ChecksumLayout;
using bess::packet::ChecksumPlan;
using bess::packet::ChecksumValues;
using bess::packet::IpVersion;
using bess::packet::NetworkChecksum;
using bess::packet::TransportChecksum;
using bess::packet::TxChecksumBackend;
using bess::packet::TxFinalizationProfile;
using bess::packet::TxOffloadCapabilities;
using bess::packet::TxTunnelEncoding;

using Bytes = std::vector<uint8_t>;

constexpr size_t kMaxPacket = 2048;
constexpr size_t kHeader = sizeof(rte_mbuf) + bess::kPacketPrivateSize;

// ---- Byte helpers ----------------------------------------------------------

uint16_t Get16(const Bytes &p, size_t at) {
  return static_cast<uint16_t>((p[at] << 8) | p[at + 1]);
}
void Put16(Bytes &p, size_t at, size_t v) {
  p[at] = static_cast<uint8_t>(v >> 8);
  p[at + 1] = static_cast<uint8_t>(v);
}
bool Within(size_t length, size_t offset, size_t size) {
  return offset <= length && size <= length - offset;
}

// Naive one's-complement sum of big-endian 16-bit words (odd tail padded).
uint64_t Sum(std::span<const uint8_t> d, uint64_t acc = 0) {
  for (size_t i = 0; i + 1 < d.size(); i += 2) {
    acc += static_cast<uint64_t>(d[i]) << 8 | d[i + 1];
  }
  if (d.size() % 2 != 0) {
    acc += static_cast<uint64_t>(d.back()) << 8;
  }
  return acc;
}
uint16_t Fold(uint64_t acc) {
  while ((acc >> 16) != 0) {
    acc = (acc & 0xffff) + (acc >> 16);
  }
  return static_cast<uint16_t>(acc);
}

// ---- Reference model -------------------------------------------------------

bool IsIpv4(const ChecksumPlan &plan) {
  return plan.ip_version == IpVersion::kIpv4;
}

bool RefPlanValid(const ChecksumPlan &plan) {
  const auto v = static_cast<uint8_t>(plan.ip_version);
  const auto n = static_cast<uint8_t>(plan.network);
  const auto t = static_cast<uint8_t>(plan.transport);
  return v <= 1 && n <= 1 && t <= 2 &&
         !(plan.network == NetworkChecksum::kIpv4Header && !IsIpv4(plan));
}

// Layout the plan must report for this packet, or nullopt when it must be
// rejected. Mirrors the documented rules: IP-declared lengths within the
// packet, unfragmented, protocol as planned, transport right after the fixed
// IP header, UDP length == transport length, TCP data offset sane.
std::optional<ChecksumLayout> RefInspect(const Bytes &p,
                                         const ChecksumPlan &plan) {
  if (!RefPlanValid(plan)) {
    return std::nullopt;
  }
  ChecksumLayout l;
  const bool want_transport = plan.transport != TransportChecksum::kNone;
  const bool udp = plan.transport == TransportChecksum::kUdp;
  const size_t no = plan.network_offset;
  size_t transport_length = 0;
  if (IsIpv4(plan)) {
    if (plan.network == NetworkChecksum::kNone && !want_transport) {
      return l;
    }
    if (!Within(p.size(), no, 20) || (p[no] >> 4) != 4 || (p[no] & 15) < 5) {
      return std::nullopt;
    }
    const size_t ihl = (p[no] & 15) * 4u;
    const size_t total = Get16(p, no + 2);
    if (total < ihl || !Within(p.size(), no, total)) {
      return std::nullopt;
    }
    l.network_header_length = ihl;
    l.network_length = total;
    l.network_checksum_offset = no + 10;
    l.transport_protocol = p[no + 9];
    if (!want_transport) {
      return l;
    }
    if (p[no + 9] != (udp ? 17 : 6) || (Get16(p, no + 6) & 0x3fff) != 0 ||
        plan.transport_offset != no + ihl) {
      return std::nullopt;
    }
    transport_length = total - ihl;
  } else {
    if (!want_transport) {
      return l;
    }
    if (!Within(p.size(), no, 40) || (p[no] >> 4) != 6) {
      return std::nullopt;
    }
    const size_t payload = Get16(p, no + 4);
    if (payload == 0 || !Within(p.size(), no, 40 + payload) ||
        p[no + 6] != (udp ? 17 : 6) || plan.transport_offset != no + 40) {
      return std::nullopt;
    }
    l.network_header_length = 40;
    l.network_length = 40 + payload;
    l.transport_protocol = p[no + 6];
    transport_length = payload;
  }
  const size_t to = plan.transport_offset;
  l.transport_length = transport_length;
  if (udp) {
    if (transport_length < 8 || Get16(p, to + 4) != transport_length) {
      return std::nullopt;
    }
    l.transport_header_length = 8;
    l.transport_checksum_offset = to + 6;
  } else {
    const size_t data_offset = transport_length < 20 ? 0 : (p[to + 12] >> 4) * 4u;
    if (data_offset < 20 || data_offset > transport_length) {
      return std::nullopt;
    }
    l.transport_header_length = data_offset;
    l.transport_checksum_offset = to + 16;
  }
  return l;
}

// One's-complement sum of the pseudo header (not complemented).
uint64_t PseudoSum(const Bytes &p, const ChecksumPlan &plan,
                   const ChecksumLayout &l) {
  const size_t no = plan.network_offset;
  Bytes pseudo;
  if (IsIpv4(plan)) {
    pseudo.assign(p.begin() + no + 12, p.begin() + no + 20);
    pseudo.insert(pseudo.end(), {0, l.transport_protocol,
                                 static_cast<uint8_t>(l.transport_length >> 8),
                                 static_cast<uint8_t>(l.transport_length)});
  } else {
    pseudo.assign(p.begin() + no + 8, p.begin() + no + 40);
    pseudo.insert(pseudo.end(),
                  {0, 0, static_cast<uint8_t>(l.transport_length >> 8),
                   static_cast<uint8_t>(l.transport_length), 0, 0, 0,
                   l.transport_protocol});
  }
  return Sum(pseudo);
}

struct RefValues {
  std::optional<uint16_t> network;
  std::optional<uint16_t> transport;
};

RefValues RefCompute(const Bytes &p, const ChecksumPlan &plan,
                     const ChecksumLayout &l) {
  RefValues v;
  if (plan.network == NetworkChecksum::kIpv4Header) {
    Bytes header(p.begin() + plan.network_offset,
                 p.begin() + plan.network_offset + l.network_header_length);
    header[10] = header[11] = 0;
    v.network = static_cast<uint16_t>(~Fold(Sum(header)));
  }
  if (plan.transport != TransportChecksum::kNone) {
    Bytes segment(p.begin() + plan.transport_offset,
                  p.begin() + plan.transport_offset + l.transport_length);
    const size_t field = l.transport_checksum_offset - plan.transport_offset;
    segment[field] = segment[field + 1] = 0;
    uint16_t c = static_cast<uint16_t>(~Fold(Sum(segment, PseudoSum(p, plan, l))));
    if (plan.transport == TransportChecksum::kUdp && c == 0) {
      c = 0xffff;
    }
    v.transport = c;
  }
  return v;
}

struct RefBound {
  TxChecksumBackend outer_network = TxChecksumBackend::kNone;
  TxChecksumBackend outer_transport = TxChecksumBackend::kNone;
  TxChecksumBackend inner_network = TxChecksumBackend::kNone;
  TxChecksumBackend inner_transport = TxChecksumBackend::kNone;
};

bool RefBindablePlan(const ChecksumPlan &plan) {
  if (!RefPlanValid(plan)) {
    return false;
  }
  if (plan.transport != TransportChecksum::kNone &&
      plan.transport_offset <= plan.network_offset) {
    return false;
  }
  return plan.network != NetworkChecksum::kNone ||
         plan.transport != TransportChecksum::kNone;
}

TxChecksumBackend Backend(bool requested, bool hardware) {
  if (!requested) {
    return TxChecksumBackend::kNone;
  }
  return hardware ? TxChecksumBackend::kHardware : TxChecksumBackend::kSoftware;
}

std::optional<RefBound> RefBind(const TxFinalizationProfile &profile,
                                const TxOffloadCapabilities &caps) {
  const auto encoding = static_cast<uint8_t>(profile.encapsulation.encoding);
  const auto outer_version =
      static_cast<uint8_t>(profile.encapsulation.outer_ip_version);
  const size_t outer_offset = profile.encapsulation.outer_network_offset;
  const bool tunneled = encoding != 0;
  if (encoding > 3) {
    return std::nullopt;
  }
  if (!tunneled ? (outer_offset != 0 || outer_version != 0) : outer_version > 1) {
    return std::nullopt;
  }
  if (!profile.outer && !profile.inner) {
    return std::nullopt;
  }
  if (profile.inner && !tunneled) {
    return std::nullopt;
  }
  if ((profile.outer && !RefBindablePlan(*profile.outer)) ||
      (profile.inner && !RefBindablePlan(*profile.inner))) {
    return std::nullopt;
  }
  const auto ttype = profile.encapsulation.encoding;
  if (tunneled && profile.outer) {
    const ChecksumPlan &o = *profile.outer;
    if (o.network_offset != outer_offset ||
        static_cast<uint8_t>(o.ip_version) != outer_version) {
      return std::nullopt;
    }
    if (o.transport != TransportChecksum::kNone &&
        (ttype == TxTunnelEncoding::kGenericIp ||
         o.transport != TransportChecksum::kUdp)) {
      return std::nullopt;
    }
  }
  if (tunneled && profile.inner &&
      profile.inner->network_offset <= outer_offset) {
    return std::nullopt;
  }

  const auto &c = caps.checksums;
  const bool encoding_ok =
      ttype == TxTunnelEncoding::kNone ||
      (ttype == TxTunnelEncoding::kGenericIp && caps.tunnel_encodings.generic_ip) ||
      (ttype == TxTunnelEncoding::kGenericUdp &&
       caps.tunnel_encodings.generic_udp) ||
      (ttype == TxTunnelEncoding::kGtp && caps.tunnel_encodings.gtp);
  RefBound b;
  if (profile.outer) {
    const ChecksumPlan &o = *profile.outer;
    const bool net = o.network == NetworkChecksum::kIpv4Header;
    const bool tr = o.transport != TransportChecksum::kNone;
    const bool udp = o.transport == TransportChecksum::kUdp;
    if (tunneled) {
      b.outer_network = Backend(net, c.outer_ipv4_header);
      b.outer_transport = Backend(tr, udp && c.outer_udp && encoding_ok);
    } else {
      b.outer_network = Backend(net, c.ipv4_header);
      b.outer_transport = Backend(tr, udp ? c.udp : c.tcp);
    }
  }
  if (profile.inner) {
    const ChecksumPlan &i = *profile.inner;
    const bool udp = i.transport == TransportChecksum::kUdp;
    b.inner_network = Backend(i.network == NetworkChecksum::kIpv4Header,
                              encoding_ok && c.ipv4_header);
    b.inner_transport = Backend(i.transport != TransportChecksum::kNone,
                                encoding_ok && (udp ? c.udp : c.tcp));
  }
  return b;
}

// ---- Chains ----------------------------------------------------------------

rte_mempool *FakePool() {
  // ValidateChain requires a pool; nothing on these paths dereferences it.
  static rte_mempool pool{};
  return &pool;
}

struct Split {
  std::vector<uint16_t> lengths;
  uint16_t headroom = 0;
  uint16_t tailroom = 0;
};

std::unique_ptr<MbufChain> Build(const Bytes &bytes, const Split *split) {
  std::vector<SegmentSpec> specs;
  if (split == nullptr) {
    specs.push_back({bytes, 0, 0});
  } else {
    specs = MbufChain::Split(bytes, split->lengths, split->headroom,
                             split->tailroom);
  }
  auto chain = std::make_unique<MbufChain>(std::move(specs));
  for (size_t i = 0; i < chain->segments(); i++) {
    chain->Mbuf(i)->pool = FakePool();
  }
  return chain;
}

// Every byte of every segment block: descriptor, private area and buffer.
std::vector<Bytes> Snapshot(const MbufChain &chain) {
  std::vector<Bytes> out;
  for (size_t i = 0; i < chain.segments(); i++) {
    const auto *base = reinterpret_cast<const uint8_t *>(chain.Mbuf(i));
    out.emplace_back(base, base + kHeader + chain.Mbuf(i)->buf_len);
  }
  return out;
}

// Writes a big-endian u16 at a logical packet offset into a snapshot.
void PatchLogical(const MbufChain &chain, std::vector<Bytes> &snap,
                  size_t offset, uint16_t value) {
  const uint8_t bytes[2] = {static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value)};
  for (uint8_t b : bytes) {
    size_t at = offset++;
    for (size_t i = 0; i < chain.segments(); i++) {
      const rte_mbuf *m = chain.Mbuf(i);
      if (at < m->data_len) {
        snap[i][kHeader + m->data_off + at] = b;
        break;
      }
      at -= m->data_len;
    }
  }
}

bool SameValues(const ChecksumValues &a, const ChecksumValues &b) {
  return a.network == b.network && a.transport == b.transport;
}
bool SameLayout(const ChecksumLayout &a, const ChecksumLayout &b) {
  return a.network_header_length == b.network_header_length &&
         a.network_length == b.network_length &&
         a.transport_length == b.transport_length &&
         a.transport_header_length == b.transport_header_length &&
         a.network_checksum_offset == b.network_checksum_offset &&
         a.transport_checksum_offset == b.transport_checksum_offset &&
         a.transport_protocol == b.transport_protocol;
}
bool Same(const std::expected<ChecksumValues, ChecksumError> &a,
          const std::expected<ChecksumValues, ChecksumError> &b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return a ? SameValues(*a, *b) : a.error() == b.error();
}
bool Same(const std::expected<ChecksumLayout, ChecksumError> &a,
          const std::expected<ChecksumLayout, ChecksumError> &b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return a ? SameLayout(*a, *b) : a.error() == b.error();
}

// ---- Decoding --------------------------------------------------------------

// Before M21, utils::CalculateSum's 16-bit loads went through a uint16_t
// pointer (UBSan at odd addresses) and this harness rounded every length and
// offset to even. checksum.h now loads with memcpy, so odd offsets and
// odd-length segments (the cross-segment pending-byte path) are fuzzed.
constexpr bool kEvenChunksOnly = false;

size_t EvenIfRequired(size_t v) { return kEvenChunksOnly ? v & ~size_t{1} : v; }

ChecksumPlan DecodePlanCtl(uint8_t ctl, FuzzInput &in) {
  ChecksumPlan plan;
  plan.ip_version = static_cast<IpVersion>(ctl & 3);
  plan.network = static_cast<NetworkChecksum>((ctl >> 2) & 3);
  plan.transport = static_cast<TransportChecksum>((ctl >> 4) & 3);
  plan.network_offset = EvenIfRequired(in.U16());
  plan.transport_offset = in.U16();
  return plan;
}

void FixLengths(Bytes &p, const ChecksumPlan &plan) {
  const size_t no = plan.network_offset;
  const bool udp = plan.transport == TransportChecksum::kUdp;
  if (IsIpv4(plan)) {
    if (!Within(p.size(), no, 20)) {
      return;
    }
    const size_t total = std::min<size_t>(p.size() - no, 0xffff);
    Put16(p, no + 2, total);
    const size_t ihl = (p[no] & 15) * 4u;
    if (udp && ihl <= total && Within(p.size(), no + ihl, 8)) {
      Put16(p, no + ihl + 4, total - ihl);
    }
  } else {
    if (!Within(p.size(), no, 40)) {
      return;
    }
    const size_t payload = std::min<size_t>(p.size() - no - 40, 0xffff);
    Put16(p, no + 4, payload);
    if (udp && Within(p.size(), no + 40, 8)) {
      Put16(p, no + 44, payload);
    }
  }
}

// ---- Checks ----------------------------------------------------------------

void CheckApply(const Bytes &bytes, const Split &split, const ChecksumPlan &plan,
                const std::expected<ChecksumValues, ChecksumError> &computed,
                const std::optional<ChecksumLayout> &ref) {
  auto chain = Build(bytes, &split);
  std::vector<Bytes> expected = Snapshot(*chain);
  bess::PacketHandle handle = chain->head_mbuf();
  const auto applied = bess::packet::ApplySoftwareChecksums(handle, plan);
  FUZZ_CHECK(handle == chain->head_mbuf());
  FUZZ_CHECK(applied.has_value() == computed.has_value());
  if (!applied) {
    FUZZ_CHECK(applied.error() == computed.error());
    FUZZ_CHECK(Snapshot(*chain) == expected);
    return;
  }
  if (computed->network) {
    PatchLogical(*chain, expected, ref->network_checksum_offset,
                 computed->network->value());
  }
  if (computed->transport) {
    PatchLogical(*chain, expected, ref->transport_checksum_offset,
                 computed->transport->value());
  }
  FUZZ_CHECK(Snapshot(*chain) == expected);
  const auto again = bess::packet::ComputeChecksums(chain->ref(), plan);
  FUZZ_CHECK(Same(again, computed));
}

uint64_t L4Flag(TransportChecksum t) {
  return t == TransportChecksum::kUdp ? RTE_MBUF_F_TX_UDP_CKSUM
                                      : RTE_MBUF_F_TX_TCP_CKSUM;
}

// Untunneled profile: full model of the result.
void CheckFinalizeUntunneled(const Bytes &bytes, const Split &split,
                             const BoundTxFinalizationProfile &bound) {
  const BoundTxChecksumDomain &domain = *bound.outer;
  const ChecksumPlan &plan = domain.plan;
  const auto layout = RefInspect(bytes, plan);

  auto chain = Build(bytes, &split);
  std::vector<Bytes> expected = Snapshot(*chain);
  bess::PacketHandle handle = chain->head_mbuf();
  const auto finalized = bess::packet::FinalizeTxPacket(handle, bound);
  FUZZ_CHECK(handle == chain->head_mbuf());
  FUZZ_CHECK(finalized.has_value() == layout.has_value());
  if (!finalized) {
    return;  // A failed packet is discarded; its bytes are unspecified.
  }
  const RefValues values = RefCompute(bytes, plan, *layout);
  const rte_mbuf *head = chain->head_mbuf();
  const bool net = plan.network != NetworkChecksum::kNone;
  const bool tr = plan.transport != TransportChecksum::kNone;
  const bool net_hw = domain.network_backend == TxChecksumBackend::kHardware;
  const bool tr_hw = domain.transport_backend == TxChecksumBackend::kHardware;
  const bool fallback =
      (net_hw || tr_hw) &&
      ((chain->segments() > 1 && !bound.multi_segment_tx) ||
       !Within(head->data_len, plan.network_offset,
               layout->network_header_length) ||
       (tr_hw && !Within(head->data_len, plan.transport_offset,
                         layout->transport_header_length)) ||
       plan.network_offset >= (size_t{1} << RTE_MBUF_L2_LEN_BITS));
  const bool eff_net_hw = net_hw && !fallback;
  const bool eff_tr_hw = tr_hw && !fallback;

  if (net) {
    PatchLogical(*chain, expected, layout->network_checksum_offset,
                 eff_net_hw ? 0 : *values.network);
  }
  if (tr) {
    PatchLogical(*chain, expected, layout->transport_checksum_offset,
                 eff_tr_hw ? Fold(PseudoSum(bytes, plan, *layout))
                           : *values.transport);
  }
  if (eff_net_hw || eff_tr_hw) {
    rte_mbuf m;
    std::memcpy(&m, expected[0].data(), sizeof(rte_mbuf));
    m.ol_flags = (IsIpv4(plan) ? RTE_MBUF_F_TX_IPV4 : RTE_MBUF_F_TX_IPV6) |
                 (eff_net_hw ? RTE_MBUF_F_TX_IP_CKSUM : 0) |
                 (eff_tr_hw ? L4Flag(plan.transport) : 0);
    m.l2_len = plan.network_offset;
    m.l3_len = layout->network_header_length;
    m.l4_len = eff_tr_hw ? layout->transport_header_length : 0;
    std::memcpy(expected[0].data(), &m, sizeof(rte_mbuf));
  }
  FUZZ_CHECK(Snapshot(*chain) == expected);
}

// Tunneled profile: offload flags only for hardware-bound fields, and the
// chain topology is unchanged.
void CheckFinalizeTunneled(const Bytes &bytes, const Split &split,
                           const BoundTxFinalizationProfile &bound) {
  auto chain = Build(bytes, &split);
  std::vector<std::array<uint32_t, 3>> topology;
  for (size_t i = 0; i < chain->segments(); i++) {
    const rte_mbuf *m = chain->Mbuf(i);
    topology.push_back({m->data_off, m->data_len, m->pkt_len});
  }
  bess::PacketHandle handle = chain->head_mbuf();
  const auto finalized = bess::packet::FinalizeTxPacket(handle, bound);
  FUZZ_CHECK(handle == chain->head_mbuf());
  for (size_t i = 0; i < chain->segments(); i++) {
    const rte_mbuf *m = chain->Mbuf(i);
    FUZZ_CHECK((topology[i] ==
                std::array<uint32_t, 3>{m->data_off, m->data_len, m->pkt_len}));
  }
  if (!finalized) {
    return;
  }
  const auto hw = [](TxChecksumBackend b) {
    return b == TxChecksumBackend::kHardware;
  };
  const bool outer_net_hw = bound.outer && hw(bound.outer->network_backend);
  const bool outer_tr_hw = bound.outer && hw(bound.outer->transport_backend);
  const bool inner_net_hw = bound.inner && hw(bound.inner->network_backend);
  const bool inner_tr_hw = bound.inner && hw(bound.inner->transport_backend);
  uint64_t allowed = 0;
  if (outer_net_hw || outer_tr_hw || inner_net_hw || inner_tr_hw) {
    allowed |= RTE_MBUF_F_TX_TUNNEL_MASK | RTE_MBUF_F_TX_OUTER_IPV4 |
               RTE_MBUF_F_TX_OUTER_IPV6;
  }
  if (outer_net_hw) {
    allowed |= RTE_MBUF_F_TX_OUTER_IP_CKSUM;
  }
  if (outer_tr_hw) {
    allowed |= RTE_MBUF_F_TX_OUTER_UDP_CKSUM;
  }
  if (inner_net_hw || inner_tr_hw) {
    allowed |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IPV6;
  }
  if (inner_net_hw) {
    allowed |= RTE_MBUF_F_TX_IP_CKSUM;
  }
  const uint64_t flags = chain->head_mbuf()->ol_flags;
  FUZZ_CHECK((flags & ~(allowed | RTE_MBUF_F_TX_L4_MASK)) == 0);
  const uint64_t l4 = flags & RTE_MBUF_F_TX_L4_MASK;
  FUZZ_CHECK(l4 == 0 || (inner_tr_hw && l4 == L4Flag(bound.inner->plan.transport)));
}

void CheckBind(FuzzInput &in, const ChecksumPlan &main_plan, const Bytes &bytes,
               const Split &split) {
  const uint8_t ctl = in.U8();
  const uint16_t cap_bits = in.U16();
  TxFinalizationProfile profile;
  profile.encapsulation.encoding = static_cast<TxTunnelEncoding>((ctl >> 3) & 7);
  profile.encapsulation.outer_ip_version = static_cast<IpVersion>(ctl >> 6);
  profile.encapsulation.outer_network_offset = in.U16();
  if ((ctl & 1) != 0) {
    profile.outer = (ctl & 4) != 0 ? DecodePlanCtl(in.U8(), in) : main_plan;
  }
  if ((ctl & 2) != 0) {
    profile.inner = DecodePlanCtl(in.U8(), in);
  }
  TxOffloadCapabilities caps;
  const auto bit = [&](unsigned i) { return ((cap_bits >> i) & 1) != 0; };
  caps.checksums.ipv4_header = bit(0);
  caps.checksums.udp = bit(1);
  caps.checksums.tcp = bit(2);
  caps.checksums.outer_ipv4_header = bit(3);
  caps.checksums.outer_udp = bit(4);
  caps.tunnel_encodings.generic_ip = bit(5);
  caps.tunnel_encodings.generic_udp = bit(6);
  caps.tunnel_encodings.gtp = bit(7);
  caps.multi_segment_tx = bit(8);

  const auto ref = RefBind(profile, caps);
  const auto bound = BindTxFinalizationProfile(profile, caps);
  FUZZ_CHECK(bound.has_value() == ref.has_value());
  if (!bound) {
    FUZZ_CHECK(bound.error() == ChecksumError::kInvalidPlan);
    return;
  }
  FUZZ_CHECK(bound->encapsulation.encoding == profile.encapsulation.encoding);
  FUZZ_CHECK(bound->encapsulation.outer_ip_version ==
             profile.encapsulation.outer_ip_version);
  FUZZ_CHECK(bound->encapsulation.outer_network_offset ==
             profile.encapsulation.outer_network_offset);
  FUZZ_CHECK(bound->multi_segment_tx == caps.multi_segment_tx);
  FUZZ_CHECK(bound->outer.has_value() == profile.outer.has_value());
  FUZZ_CHECK(bound->inner.has_value() == profile.inner.has_value());
  const auto same_plan = [](const ChecksumPlan &a, const ChecksumPlan &b) {
    return a.network_offset == b.network_offset &&
           a.transport_offset == b.transport_offset &&
           a.ip_version == b.ip_version && a.network == b.network &&
           a.transport == b.transport;
  };
  if (bound->outer) {
    FUZZ_CHECK(same_plan(bound->outer->plan, *profile.outer));
    FUZZ_CHECK(bound->outer->network_backend == ref->outer_network);
    FUZZ_CHECK(bound->outer->transport_backend == ref->outer_transport);
  }
  if (bound->inner) {
    FUZZ_CHECK(same_plan(bound->inner->plan, *profile.inner));
    FUZZ_CHECK(bound->inner->network_backend == ref->inner_network);
    FUZZ_CHECK(bound->inner->transport_backend == ref->inner_transport);
  }

  if (profile.encapsulation.encoding == TxTunnelEncoding::kNone) {
    CheckFinalizeUntunneled(bytes, split, *bound);
  } else {
    CheckFinalizeTunneled(bytes, split, *bound);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput in(data, size);
  const uint8_t plan_ctl = in.U8();
  ChecksumPlan plan = DecodePlanCtl(plan_ctl, in);

  Split split;
  const size_t cuts = in.U8() % 8;
  for (size_t i = 0; i < cuts; i++) {
    split.lengths.push_back(static_cast<uint16_t>(EvenIfRequired(in.U8())));
  }
  split.headroom = static_cast<uint16_t>(EvenIfRequired(in.U8()));
  split.tailroom = in.U8();

  // The profile is decoded after the packet bytes are known; keep its bytes.
  const auto profile_head = in.Bytes(5);
  const uint8_t profile_ctl = profile_head.empty() ? 0 : profile_head[0];
  const size_t profile_extra =
      ((profile_ctl & 5) == 5 ? 5 : 0) + ((profile_ctl & 2) != 0 ? 5 : 0);
  const auto profile_tail = in.Bytes(profile_extra);
  Bytes profile_bytes(profile_head.begin(), profile_head.end());
  profile_bytes.insert(profile_bytes.end(), profile_tail.begin(),
                       profile_tail.end());

  const auto rest = in.Rest();
  Bytes bytes(rest.begin(), rest.begin() + std::min(rest.size(), kMaxPacket));
  if ((plan_ctl & 0x40) != 0) {
    FixLengths(bytes, plan);
  }
  if ((plan_ctl & 0x80) != 0) {
    const size_t no = plan.network_offset;
    plan.transport_offset =
        no + (IsIpv4(plan) ? (no < bytes.size() ? (bytes[no] & 15) * 4u : 0)
                           : 40u);
  }

  const auto ref = RefInspect(bytes, plan);
  const auto flat = Build(bytes, nullptr);
  const auto chain = Build(bytes, &split);
  const auto inspect_flat = bess::packet::InspectChecksumPlan(flat->ref(), plan);
  const auto inspect = bess::packet::InspectChecksumPlan(chain->ref(), plan);
  const auto compute_flat = bess::packet::ComputeChecksums(flat->ref(), plan);
  const auto compute = bess::packet::ComputeChecksums(chain->ref(), plan);

  FUZZ_CHECK(Same(inspect, inspect_flat));
  FUZZ_CHECK(Same(compute, compute_flat));
  FUZZ_CHECK(inspect.has_value() == compute.has_value());
  FUZZ_CHECK(inspect.has_value() || inspect.error() == compute.error());
  FUZZ_CHECK(inspect.has_value() == ref.has_value());
  if (ref) {
    FUZZ_CHECK(SameLayout(*inspect, *ref));
    const RefValues values = RefCompute(bytes, plan, *ref);
    FUZZ_CHECK(compute->network.has_value() == values.network.has_value());
    FUZZ_CHECK(compute->transport.has_value() == values.transport.has_value());
    FUZZ_CHECK(!values.network || compute->network->value() == *values.network);
    FUZZ_CHECK(!values.transport ||
               compute->transport->value() == *values.transport);
  }
  // Read-only: the chain bytes are still the input.
  FUZZ_CHECK(chain->Flatten() == bytes);

  CheckApply(bytes, split, plan, compute, ref);

  FuzzInput profile_in(profile_bytes.data(), profile_bytes.size());
  CheckBind(profile_in, plan, bytes, split);
  return 0;
}
