// SPDX-License-Identifier: BSD-3-Clause

// The M13 cases (roadmap: NAT rewrite, VXLAN encap/decap, a VFP-like
// multi-field rewrite, a UPF-like outer-header replacement, a one-write plan),
// shared by packet_edit_plan_test.cc and packet_edit_plan_bench.cc: the input
// packet, the hand-written C++ for each, a typed per-flow action, and the
// EditPlan. Test and benchmark support; not installed.

#ifndef BESS_PACKET_EDIT_PLAN_CASES_H_
#define BESS_PACKET_EDIT_PLAN_CASES_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rte_byteorder.h>
#include <rte_mbuf.h>

#include "packet.h"
#include "packet_edit_plan.h"
#include "utils/checksum.h"
#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"

namespace bess::packet::edit_cases {

using utils::be16_t;
using utils::be32_t;

// Offsets in an Ethernet + IPv4 (no options) + TCP packet.
constexpr uint16_t kIp = 14, kIpLen = 16, kIpCsum = 24, kIpSrc = 26, kIpDst = 30;
constexpr uint16_t kL4 = 34, kSport = 34, kTcpCsum = 50;
constexpr uint16_t kVxlanOuter = 50;  // Eth 14 + IPv4 20 + UDP 8 + VXLAN 8
constexpr uint16_t kGtpOuter = 50;    // Eth 14 + IPv4 20 + UDP 8 + GTP-U 8

// A 128-byte TCP packet with valid checksums.
inline std::vector<uint8_t> TcpPacket() {
  std::vector<uint8_t> b(128, 0);
  const uint8_t mac[12] = {2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 2};
  std::memcpy(b.data(), mac, 12);
  b[12] = 0x08;
  auto *ip = reinterpret_cast<utils::Ipv4 *>(b.data() + kIp);
  ip->version = 4;
  ip->header_length = 5;
  ip->length = be16_t(static_cast<uint16_t>(b.size() - kIp));
  ip->ttl = 64;
  ip->protocol = utils::Ipv4::Proto::kTcp;
  ip->src = be32_t(0x0a000001);
  ip->dst = be32_t(0xc0a80001);
  auto *tcp = reinterpret_cast<utils::Tcp *>(b.data() + kL4);
  tcp->src_port = be16_t(40000);
  tcp->dst_port = be16_t(443);
  tcp->offset = 5;
  for (size_t i = kL4 + 20; i < b.size(); i++) {
    b[i] = static_cast<uint8_t>(i * 7);
  }
  ip->checksum = utils::CalculateIpv4NoOptChecksum(*ip);
  tcp->checksum = utils::CalculateIpv4TcpChecksum(*ip, *tcp);
  return b;
}

// -- NAT: source address and port rewrite with incremental checksums --------

struct Nat {
  be32_t old_ip, new_ip;
  be16_t old_port, new_port;
};
inline Nat NatParams() {
  return {be32_t(0x0a000001), be32_t(0xcb007101), be16_t(40000), be16_t(61001)};
}

// Hand-written, as a NAT module would.
inline void NatHand(uint8_t *p, const Nat &n) {
  auto *ip = reinterpret_cast<utils::Ipv4 *>(p + kIp);
  auto *tcp = reinterpret_cast<utils::Tcp *>(p + kL4);
  const uint32_t l3 = utils::ChecksumIncrement32(ip->src.raw_value(), n.new_ip.raw_value());
  const uint32_t l4 = l3 + utils::ChecksumIncrement16(tcp->src_port.raw_value(),
                                                      n.new_port.raw_value());
  ip->src = n.new_ip;
  tcp->src_port = n.new_port;
  ip->checksum = utils::UpdateChecksumWithIncrement(ip->checksum, l3);
  tcp->checksum = utils::UpdateChecksumWithIncrement(tcp->checksum, l4);
}

// A typed per-flow action: the increments are computed once, at flow setup.
struct NatAction {
  be32_t new_ip;
  be16_t new_port;
  uint32_t l3_inc, l4_inc;
  static NatAction For(const Nat &n) {
    const uint32_t l3 = utils::ChecksumIncrement32(n.old_ip.raw_value(), n.new_ip.raw_value());
    return {n.new_ip, n.new_port, l3,
            l3 + utils::ChecksumIncrement16(n.old_port.raw_value(), n.new_port.raw_value())};
  }
  void Apply(uint8_t *p) const {
    auto *ip = reinterpret_cast<utils::Ipv4 *>(p + kIp);
    auto *tcp = reinterpret_cast<utils::Tcp *>(p + kL4);
    ip->src = new_ip;
    tcp->src_port = new_port;
    ip->checksum = utils::UpdateChecksumWithIncrement(ip->checksum, l3_inc);
    tcp->checksum = utils::UpdateChecksumWithIncrement(tcp->checksum, l4_inc);
  }
};

inline EditPlan NatPlan(const Nat &n) {
  const auto bytes = [](const auto &v) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&v), sizeof(v));
  };
  return EditPlanBuilder()
      .Write(kIpSrc, bytes(n.new_ip))
      .Write(kSport, bytes(n.new_port))
      .AdjustChecksum(kIpCsum, bytes(n.old_ip), bytes(n.new_ip))
      .AdjustChecksum(kTcpCsum, bytes(n.old_ip), bytes(n.new_ip))
      .AdjustChecksum(kTcpCsum, bytes(n.old_port), bytes(n.new_port))
      .Build()
      .value();
}

// -- VXLAN: outer Ethernet + IPv4 + UDP + VXLAN header ------------------------

inline std::array<uint8_t, kVxlanOuter> VxlanHeader() {
  std::array<uint8_t, kVxlanOuter> h{};
  const uint8_t mac[12] = {2, 0, 0, 0, 0, 0x0a, 2, 0, 0, 0, 0, 0x0b};
  std::memcpy(h.data(), mac, 12);
  h[12] = 0x08;
  auto *ip = reinterpret_cast<utils::Ipv4 *>(h.data() + 14);
  ip->version = 4;
  ip->header_length = 5;
  ip->ttl = 64;
  ip->protocol = utils::Ipv4::Proto::kUdp;
  ip->src = be32_t(0x0a0a0001);
  ip->dst = be32_t(0x0a0a0002);
  auto *udp = reinterpret_cast<utils::Udp *>(h.data() + 34);
  udp->src_port = be16_t(49152);
  udp->dst_port = be16_t(4789);
  h[42] = 0x08;          // VXLAN flags: VNI valid
  h[46] = 0x00;
  h[47] = 0x00;
  h[45] = 0x2a;          // VNI 42 (bytes 44..46)
  return h;
}

inline void VxlanEncapHand(rte_mbuf *m, const std::array<uint8_t, kVxlanOuter> &h) {
  auto *p = reinterpret_cast<uint8_t *>(rte_pktmbuf_prepend(m, kVxlanOuter));
  std::memcpy(p, h.data(), kVxlanOuter);
  auto *ip = reinterpret_cast<utils::Ipv4 *>(p + 14);
  auto *udp = reinterpret_cast<utils::Udp *>(p + 34);
  ip->length = be16_t(static_cast<uint16_t>(m->pkt_len - 14));
  udp->length = be16_t(static_cast<uint16_t>(m->pkt_len - 34));
  ip->checksum = utils::CalculateIpv4NoOptChecksum(*ip);
}

inline EditPlan VxlanEncapPlan(const std::array<uint8_t, kVxlanOuter> &h) {
  return EditPlanBuilder()
      .Prepend(h)
      .SetLength(16, 14)
      .SetLength(38, 34)
      .Ipv4HeaderChecksum(14)
      .Build()
      .value();
}

inline void VxlanDecapHand(rte_mbuf *m) { rte_pktmbuf_adj(m, kVxlanOuter); }
inline EditPlan VxlanDecapPlan() {
  return EditPlanBuilder().RemovePrefix(kVxlanOuter).Build().value();
}

// -- VFP-like: rewrite both MACs, the destination address and port -----------
// (A DSCP rewrite is not in this case: TOS is not part of the flow match, so its
// old value is per packet and a precomputed checksum delta cannot cover it.)

struct Vfp {
  uint8_t macs[12];
  be32_t old_dst, new_dst;
  be16_t old_dport, new_dport;
};
inline Vfp VfpParams() {
  return {{0, 0x1d, 0xd8, 0, 0, 1, 0, 0x1d, 0xd8, 0, 0, 2},
          be32_t(0xc0a80001), be32_t(0x0a640005), be16_t(443), be16_t(8443)};
}

inline void VfpHand(uint8_t *p, const Vfp &v) {
  std::memcpy(p, v.macs, 12);
  auto *ip = reinterpret_cast<utils::Ipv4 *>(p + kIp);
  auto *tcp = reinterpret_cast<utils::Tcp *>(p + kL4);
  const uint32_t addr = utils::ChecksumIncrement32(ip->dst.raw_value(), v.new_dst.raw_value());
  const uint32_t port = utils::ChecksumIncrement16(tcp->dst_port.raw_value(), v.new_dport.raw_value());
  ip->dst = v.new_dst;
  tcp->dst_port = v.new_dport;
  ip->checksum = utils::UpdateChecksumWithIncrement(ip->checksum, addr);
  tcp->checksum = utils::UpdateChecksumWithIncrement(tcp->checksum, addr + port);
}

inline EditPlan VfpPlan(const Vfp &v) {
  const auto bytes = [](const auto &x) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&x), sizeof(x));
  };
  return EditPlanBuilder()
      .Write(0, std::span<const uint8_t>(v.macs, 12))
      .Write(kIpDst, bytes(v.new_dst))
      .Write(kL4 + 2, bytes(v.new_dport))
      .AdjustChecksum(kIpCsum, bytes(v.old_dst), bytes(v.new_dst))
      .AdjustChecksum(kTcpCsum, bytes(v.old_dst), bytes(v.new_dst))
      .AdjustChecksum(kTcpCsum, bytes(v.old_dport), bytes(v.new_dport))
      .Build()
      .value();
}

// -- UPF-like: replace the outer Ethernet + IPv4 + UDP + GTP-U with Ethernet --

inline std::array<uint8_t, 14> InnerEthernet() {
  return {2, 0, 0, 0, 0, 0x0c, 2, 0, 0, 0, 0, 0x0d, 0x08, 0x00};
}
inline void UpfHand(rte_mbuf *m, const std::array<uint8_t, 14> &eth) {
  rte_pktmbuf_adj(m, kGtpOuter);
  auto *p = reinterpret_cast<uint8_t *>(rte_pktmbuf_prepend(m, 14));
  std::memcpy(p, eth.data(), 14);
}
inline EditPlan UpfPlan(const std::array<uint8_t, 14> &eth) {
  return EditPlanBuilder().RemovePrefix(kGtpOuter).Prepend(eth).Build().value();
}

// -- one write ------------------------------------------------------------------

inline EditPlan OneWritePlan() {
  const uint8_t ttl = 32;
  return EditPlanBuilder().Write(22, std::span<const uint8_t>(&ttl, 1)).Build().value();
}

}  // namespace bess::packet::edit_cases

#endif  // BESS_PACKET_EDIT_PLAN_CASES_H_
