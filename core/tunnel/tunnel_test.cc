// SPDX-License-Identifier: BSD-3-Clause

// Tunnel packet mechanics (M19, D-069).

#include "tunnel/tunnel.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <random>
#include <vector>

namespace bess::tunnel {
namespace {

using utils::be32_t;

// The outer parse of the last decapsulation, and wrappers that keep it.
// They return std::expected for readable tests.
conntrack::ParsedFlowPacket g_outer;
template <typename R, typename Fn>
std::expected<R, DecapError> Wrap(Fn fn) {
  R r{};
  const DecapError e = fn(r);
  if (e != DecapError::kOk) return std::unexpected(e);
  return r;
}
auto Vx(std::span<const uint8_t> f, std::optional<uint16_t> port = kVxlanPort, size_t total = 0) {
  return Wrap<Decapsulated>([&](Decapsulated &r) { return DecapVxlan(f, g_outer, r, port, total); });
}
auto Gv(std::span<const uint8_t> f) {
  return Wrap<GeneveInfo>([&](GeneveInfo &r) { return DecapGeneve(f, g_outer, r); });
}
auto Gr(std::span<const uint8_t> f) {
  return Wrap<Decapsulated>([&](Decapsulated &r) { return DecapGre(f, g_outer, r); });
}
auto Gt(std::span<const uint8_t> f) {
  return Wrap<GtpuInfo>([&](GtpuInfo &r) { return DecapGtpu(f, g_outer, r); });
}
auto PGre(std::span<const uint8_t> f, size_t off) {
  return Wrap<GreInfo>([&](GreInfo &r) { return ParseGre(f, off, r); });
}

// An inner Ethernet/IPv4/UDP frame.
std::vector<uint8_t> Inner(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, uint16_t ip_id = 1,
                           size_t payload = 20) {
  std::vector<uint8_t> f(kEthernetBytes + kIpv4Bytes + kUdpBytes + payload, 0);
  f[0] = 0x02;
  f[6] = 0x02;
  f[11] = 1;
  detail::Put16(f.data() + 12, 0x0800);
  WriteIpv4(f.data() + 14, be32_t(src), be32_t(dst), 17, static_cast<uint16_t>(kUdpBytes + payload));
  detail::Put16(f.data() + 18, ip_id);
  WriteUdp(f.data() + 34, sport, dport, static_cast<uint16_t>(payload));
  return f;
}

// Outer Ethernet (+ optional VLAN) + IPv4 + UDP + `tunnel` header bytes + inner.
std::vector<uint8_t> OuterUdp(uint16_t dport, std::vector<uint8_t> tunnel_header,
                              const std::vector<uint8_t> &inner, bool vlan = false) {
  std::vector<uint8_t> f(kEthernetBytes + (vlan ? 4 : 0), 0);
  if (vlan) {
    detail::Put16(f.data() + 12, 0x8100);
    detail::Put16(f.data() + 14, 7);
    detail::Put16(f.data() + 16, 0x0800);
  } else {
    detail::Put16(f.data() + 12, 0x0800);
  }
  const size_t ip = f.size();
  const size_t udp_payload = tunnel_header.size() + inner.size();
  f.resize(ip + kIpv4Bytes + kUdpBytes);
  WriteIpv4(f.data() + ip, be32_t(0xc0000201), be32_t(0xc0000202), 17,
            static_cast<uint16_t>(kUdpBytes + udp_payload));
  WriteUdp(f.data() + ip + kIpv4Bytes, 0xc123, dport, static_cast<uint16_t>(udp_payload));
  f.insert(f.end(), tunnel_header.begin(), tunnel_header.end());
  f.insert(f.end(), inner.begin(), inner.end());
  return f;
}

std::vector<uint8_t> VxlanHeader(uint32_t vni, uint8_t flags = 0x08) {
  std::vector<uint8_t> h(8, 0);
  h[0] = flags;
  detail::Put32(h.data() + 4, vni << 8);
  return h;
}

TEST(TunnelTest, VxlanEncapDecapRoundTrip) {
  const auto inner = Inner(0x0a000001, 1000, 0x0a000002, 2000);
  // Encapsulate as VXLANEncap does: UDP + VXLAN in front of the frame.
  std::vector<uint8_t> hdr(16);
  WriteVxlanUdp(hdr.data(), 0x123456, FlowEntropyPort(inner), kVxlanPort, inner.size());
  std::vector<uint8_t> udp_vx(hdr.begin() + 8, hdr.end());
  for (const bool vlan : {false, true}) {
    const auto frame = OuterUdp(kVxlanPort, udp_vx, inner, vlan);
    const auto d = Vx(frame);
    ASSERT_TRUE(d.has_value()) << vlan;
    EXPECT_EQ(0x123456u, d->id);
    EXPECT_EQ(frame.size() - inner.size(), d->inner_offset);
    EXPECT_EQ(0, std::memcmp(frame.data() + d->inner_offset, inner.data(), inner.size()));
    EXPECT_EQ(0xc0, g_outer.src[0]);
    EXPECT_TRUE(utils::VerifyIpv4NoOptChecksum(
        *reinterpret_cast<const utils::Ipv4 *>(frame.data() + g_outer.l3_offset)));
  }
  // The UDP length written covers the VXLAN header and the inner frame.
  EXPECT_EQ(8 + 8 + inner.size(), detail::Get16(hdr.data() + 4));
  EXPECT_EQ(0x08, hdr[8]);
  EXPECT_EQ(0x12, hdr[12]);
  EXPECT_EQ(0x56, hdr[14]);
  EXPECT_EQ(0, hdr[15]);
}

// A chained packet: the outer and VXLAN headers in the first segment, the
// inner frame continuing past it.
TEST(TunnelTest, VxlanDecapFromTheFirstSegment) {
  const auto inner = Inner(1, 1, 2, 2, 1, 400);
  const auto frame = OuterUdp(kVxlanPort, VxlanHeader(9), inner);
  const std::span<const uint8_t> head(frame.data(), 80);
  EXPECT_EQ(DecapError::kMalformed, Vx(head).error()) << "alone, the lengths lie";
  const auto d = Vx(head, kVxlanPort, frame.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(9u, d->id);
  EXPECT_EQ(50u, d->inner_offset);
  EXPECT_EQ(DecapError::kBadHeader,
            Vx(std::span<const uint8_t>(frame.data(), 45), kVxlanPort, frame.size()).error())
      << "the VXLAN header must be readable";
}

TEST(TunnelTest, VxlanDecapRefusesWhatIsNotAValidTunnel) {
  const auto inner = Inner(1, 1, 2, 2);
  auto frame = OuterUdp(kVxlanPort, VxlanHeader(7), inner);
  auto fragmented = frame;
  detail::Put16(fragmented.data() + 20, 0x2000);  // the outer IPv4 header: MF, offset 0
  fragmented[24] = fragmented[25] = 0;
  const uint16_t sum = utils::CalculateIpv4NoOptChecksum(*reinterpret_cast<const utils::Ipv4 *>(fragmented.data() + 14));
  std::memcpy(fragmented.data() + 24, &sum, 2);
  EXPECT_EQ(DecapError::kMalformed, Vx(fragmented).error()) << "an outer first fragment";
  EXPECT_EQ(DecapError::kNotTunnel, Vx(frame, 4790).error());
  EXPECT_TRUE(Vx(frame, std::nullopt).has_value()) << "any port when classified upstream";
  EXPECT_EQ(DecapError::kBadHeader, Vx(OuterUdp(kVxlanPort, VxlanHeader(7, 0), inner)).error())
      << "I flag clear";
  EXPECT_EQ(DecapError::kNoInner,
            Vx(OuterUdp(kVxlanPort, VxlanHeader(7), std::vector<uint8_t>(13))).error());
  // Every truncation fails cleanly (the outer lengths then lie).
  for (size_t n = 0; n < frame.size(); n++) {
    std::vector<uint8_t> cut(frame.begin(), frame.begin() + n);
    ASSERT_FALSE(Vx(cut).has_value()) << n;
  }
}

TEST(TunnelTest, EntropyPortIsPerFlowAndInTheDynamicRange) {
  std::mt19937 rng(19);
  for (int i = 0; i < 1000; i++) {
    const uint32_t a = rng(), b = rng();
    const uint16_t sp = static_cast<uint16_t>(rng()), dp = static_cast<uint16_t>(rng());
    const uint16_t p1 = FlowEntropyPort(Inner(a, sp, b, dp, /*ip_id=*/1));
    const uint16_t p2 = FlowEntropyPort(Inner(a, sp, b, dp, /*ip_id=*/static_cast<uint16_t>(rng())));
    ASSERT_GE(p1, 0xc000);
    ASSERT_EQ(p1, p2) << "the IP identification must not change a flow's port";
  }
  // Inner ports are part of the flow: two flows that differ only in ports
  // spread.
  int differ = 0;
  for (uint16_t s = 0; s < 64; s++) {
    differ += FlowEntropyPort(Inner(1, s, 2, 80)) != FlowEntropyPort(Inner(1, 1000, 2, 80));
  }
  EXPECT_GT(differ, 50);
  // A datagram's fragments share the port: the bytes at the L4 offset of a
  // later fragment are payload, not ports.
  auto first = Inner(1, 1111, 2, 2222);
  detail::Put16(first.data() + 20, 0x2000);  // MF, offset 0
  auto later = Inner(1, 0x4142, 2, 0x4344);   // payload bytes where ports would be
  detail::Put16(later.data() + 20, 0x00b9);  // offset 185 * 8
  EXPECT_EQ(FlowEntropyPort(first), FlowEntropyPort(later));
  // Non-IPv4 frames hash their MACs; short frames do not read past the end.
  std::vector<uint8_t> arp(42, 0);
  detail::Put16(arp.data() + 12, 0x0806);
  EXPECT_GE(FlowEntropyPort(arp), 0xc000);
  EXPECT_EQ(0xc000, FlowEntropyPort(std::vector<uint8_t>(5)));
  const auto whole = Inner(1, 1, 2, 2);
  const auto shortip = std::vector<uint8_t>(whole.begin(), whole.begin() + 36);
  EXPECT_GE(FlowEntropyPort(shortip), 0xc000);
}

TEST(TunnelTest, GeneveOptionsAndCriticalBit) {
  const auto inner = Inner(1, 1, 2, 2);
  std::vector<uint8_t> g(8 + 8, 0);
  ASSERT_TRUE(WriteGeneve(g.data(), 0xabcdef, 0x6558, 8, false, true));
  g[8] = 0x01;  // an option's class
  auto frame = OuterUdp(kGenevePort, g, inner);
  const auto d = Gv(frame);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(0xabcdefu, d->tunnel.id);
  EXPECT_EQ(0x6558, d->tunnel.protocol);
  EXPECT_EQ(8u, d->options_bytes);
  EXPECT_TRUE(d->critical);
  EXPECT_FALSE(d->oam);
  EXPECT_EQ(frame.size() - inner.size(), d->tunnel.inner_offset);
  EXPECT_FALSE(WriteGeneve(g.data(), 1, 0x6558, 6)) << "options are 4-byte words";
  g[0] = 0x40 | 2;  // version 1
  EXPECT_EQ(DecapError::kBadHeader, Gv(OuterUdp(kGenevePort, g, inner)).error());
  g[0] = 63;  // 252 option bytes, beyond the packet
  EXPECT_EQ(DecapError::kBadHeader, Gv(OuterUdp(kGenevePort, g, {})).error());
  // Options that run past the UDP datagram into Ethernet padding: the frame
  // holds the bytes, the packet does not.
  std::vector<uint8_t> base(8, 0);
  ASSERT_TRUE(WriteGeneve(base.data(), 1, 0x0800, 0));
  base[0] = 2;  // claims 8 option bytes the datagram does not carry
  auto padded = OuterUdp(kGenevePort, base, {});
  padded.resize(padded.size() + 16, 0);
  EXPECT_EQ(DecapError::kBadHeader, Gv(padded).error());
}

TEST(TunnelTest, GreKeySequenceAndChecksum) {
  const auto frame_in = Inner(1, 1, 2, 2);
  const auto payload = std::vector<uint8_t>(frame_in.begin() + 14, frame_in.end());
  GreOptions o;
  o.key = 0xdeadbeef;
  o.sequence = 42;
  o.checksum = true;
  std::vector<uint8_t> bytes(GreBytes(o));
  ASSERT_EQ(16u, WriteGre(bytes.data(), 0x0800, o, payload));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  auto g = PGre(bytes, 0);
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(0x0800, g->protocol);
  EXPECT_EQ(0xdeadbeefu, g->key.value());
  EXPECT_EQ(42u, g->sequence.value());
  EXPECT_EQ(16u, g->payload_offset);
  bytes.back() ^= 1;
  EXPECT_FALSE(PGre(bytes, 0).has_value()) << "a corrupted payload fails the checksum";
  // No options; routing (and versions) refused.
  std::vector<uint8_t> plain(4);
  WriteGre(plain.data(), 0x86dd, {});
  EXPECT_EQ(4u, PGre(plain, 0)->payload_offset);
  plain[0] = 0x40;
  EXPECT_FALSE(PGre(plain, 0).has_value()) << "routing";
  plain[0] = 0x08;
  EXPECT_FALSE(PGre(plain, 0).has_value()) << "strict source route";
  plain[0] = 0x04;
  EXPECT_FALSE(PGre(plain, 0).has_value()) << "recursion";
  // Over IPv4 (protocol 47).
  std::vector<uint8_t> frame(14 + 20, 0);
  detail::Put16(frame.data() + 12, 0x0800);
  std::vector<uint8_t> gre(4);
  WriteGre(gre.data(), 0x0800, {});
  WriteIpv4(frame.data() + 14, be32_t(1), be32_t(2), 47, static_cast<uint16_t>(4 + payload.size()));
  frame.insert(frame.end(), gre.begin(), gre.end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  const auto d = Gr(frame);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(38u, d->inner_offset);
  EXPECT_EQ(0x0800, d->protocol);
}

std::vector<uint8_t> Gtpu(uint8_t flags, uint8_t type, std::vector<uint8_t> optional_and_ext, size_t payload) {
  std::vector<uint8_t> h(8, 0);
  h[0] = static_cast<uint8_t>(0x30 | flags);
  h[1] = type;
  detail::Put16(h.data() + 2, static_cast<uint16_t>(optional_and_ext.size() + payload));
  detail::Put32(h.data() + 4, 0x11223344);
  h.insert(h.end(), optional_and_ext.begin(), optional_and_ext.end());
  return h;
}

TEST(TunnelTest, GtpuHeaderCodecOnly) {
  const auto frame_in = Inner(1, 1, 2, 2);
  const auto inner = std::vector<uint8_t>(frame_in.begin() + 14, frame_in.end());
  std::vector<uint8_t> min(8);
  WriteGtpu(min.data(), 0x11223344, static_cast<uint16_t>(inner.size()));
  auto d = Gt(OuterUdp(kGtpuPort, min, inner));
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(0x11223344u, d->tunnel.id);
  EXPECT_EQ(14u + 20u + 8u + 8u, d->tunnel.inner_offset);
  EXPECT_FALSE(d->first_extension.has_value());
  // S and E: a sequence number and one 4-byte extension header (the PDU
  // session container's type, 0x85, is just a number here).
  std::vector<uint8_t> opt = {0x00, 0x07, 0x00, 0x85, 0x01, 0x10, 0x09, 0x00};
  d = Gt(OuterUdp(kGtpuPort, Gtpu(0x06, 255, opt, inner.size()), inner));
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(7, d->sequence.value());
  EXPECT_EQ(0x85, d->first_extension.value());
  EXPECT_EQ(14u + 20u + 8u + 8u + 8u, d->tunnel.inner_offset);
  // An extension length of 0, one past the message, a non-G-PDU, GTP'.
  auto err = [](const auto &r) { return r.has_value() ? static_cast<DecapError>(99) : r.error(); };
  opt[4] = 0;
  EXPECT_EQ(DecapError::kBadHeader, err(Gt(OuterUdp(kGtpuPort, Gtpu(0x06, 255, opt, inner.size()), inner))));
  opt[4] = 30;  // 120 bytes: past the end of the message
  EXPECT_EQ(DecapError::kBadHeader, err(Gt(OuterUdp(kGtpuPort, Gtpu(0x06, 255, opt, inner.size()), inner))));
  EXPECT_EQ(DecapError::kNotTunnel, err(Gt(OuterUdp(kGtpuPort, Gtpu(0, 1, {}, 0), {})))) << "echo request";
  auto prime = Gtpu(0, 255, {}, inner.size());
  prime[0] = 0x20;  // PT 0
  EXPECT_EQ(DecapError::kBadHeader, err(Gt(OuterUdp(kGtpuPort, prime, inner))));
  auto lie = Gtpu(0, 255, {}, inner.size() + 1);
  EXPECT_EQ(DecapError::kBadHeader, err(Gt(OuterUdp(kGtpuPort, lie, inner))));
}

TEST(TunnelTest, OverheadAndMtu) {
  EXPECT_EQ(50u, VxlanOverhead(Outer::kIpv4));
  EXPECT_EQ(70u, VxlanOverhead(Outer::kIpv6));
  EXPECT_EQ(1464u, InnerMtu(1500, VxlanOverhead(Outer::kIpv4))) << "an inner frame, its Ethernet header included";
  EXPECT_EQ(58u, GeneveOverhead(Outer::kIpv4, 8));
  EXPECT_EQ(0u, InnerMtu(30, VxlanOverhead(Outer::kIpv4)));
}

}  // namespace
}  // namespace bess::tunnel
