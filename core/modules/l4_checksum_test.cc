// SPDX-License-Identifier: BSD-3-Clause

// L4Checksum in process: the TCP and UDP checksums over IPv4 are recomputed
// (or verified) where the checked parse finds the headers, over the datagram
// the IP header states, across a chain's segments; every other frame passes
// unchanged, a frame whose headers do not fit its first segment or lie about
// their lengths is dropped, and a shared payload is copied first (D-153).
// The reference checksums are PacketBuilder's own (its one's-complement sum),
// not the library's the module uses.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "packet.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::PacketBytes;

using Gates = std::map<gate_idx_t, std::vector<Bytes>>;

constexpr std::string_view kPayload = "helloworldhelloworldhelloworld!";  // odd length
constexpr uint16_t kWrong = 0x1234;

// eth / `tags` VLAN tags (0, 802.1Q, 802.1ad + 802.1Q) / IPv4 (`frag`,
// `options` bytes of NOP options) / TCP or UDP / `payload`, the L4 checksum
// the builder's (the reference) unless `sum` gives one.
Bytes Frame(bool tcp, int tags, std::optional<uint16_t> sum = std::nullopt,
            std::string_view payload = kPayload, uint16_t frag = 0, size_t options = 0) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01");
  if (tags == 1) {
    b.Dot1Q(6);
  } else if (tags == 2) {
    b.Dot1AD(5).Dot1Q(6);
  }
  b.IPv4("10.0.0.1", "10.0.0.2");
  b.Last<PacketBuilder::IPv4Hdr>().frag = frag;
  b.Last<PacketBuilder::IPv4Hdr>().options = Bytes(options, 0x01);
  if (tcp) {
    b.Tcp(1000, 2000);
    b.Last<PacketBuilder::TcpHdr>().checksum = sum;
  } else {
    b.Udp(1000, 2000);
    b.Last<PacketBuilder::UdpHdr>().checksum = sum;
  }
  return b.Payload(payload).Build();
}

// A frame's header shape: VLAN tags and IPv4 option bytes (IHL 5, 6 or 7).
struct Shape {
  int tags;
  size_t options;
};
constexpr Shape kShapes[] = {{0, 0}, {1, 0}, {2, 0}, {0, 8}, {2, 4}};

Bytes Frame(bool tcp, const Shape &s, std::optional<uint16_t> sum = std::nullopt) {
  return Frame(tcp, s.tags, sum, kPayload, 0, s.options);
}

size_t L3(int tags) { return 14 + 4 * static_cast<size_t>(tags); }
size_t L4(const Shape &s) { return L3(s.tags) + 20 + s.options; }

uint16_t Get16(const Bytes &b, size_t offset) {
  return static_cast<uint16_t>(unsigned{b[offset]} << 8u | b[offset + 1]);
}

// Writes `value` big-endian at `offset`.
void Set16(Bytes &b, size_t offset, size_t value) {
  b[offset] = static_cast<uint8_t>(value >> 8u);
  b[offset + 1] = static_cast<uint8_t>(value & 0xffu);
}

// `frame`, its right checksum at `field` made 0x0000 by adding it (one's
// complement) to the summed word at `word`, then stored as 0xffff: -0, as
// an incremental update may leave it (RFC 1624), and as right as 0x0000.
// (A UDP checksum computed as 0x0000 is always sent as 0xffff.)
Bytes MinusZero(Bytes frame, size_t word, size_t field) {
  uint32_t sum = uint32_t{Get16(frame, word)} + Get16(frame, field);
  sum = (sum & 0xffffu) + (sum >> 16u);
  Set16(frame, word, sum);
  Set16(frame, field, 0xffff);
  return frame;
}

std::string Name(bool tcp, const Shape &s) {
  return std::string(tcp ? "tcp" : "udp") + " tags " + std::to_string(s.tags) + " options " +
         std::to_string(s.options);
}

Module *Create(ModuleHarness &h, bool verify) {
  bess::pb::L4ChecksumArg arg;
  arg.set_verify(verify);
  return h.Create("L4Checksum", arg);
}

// `packets` through `m`: what left on gates 0 and 1.
Gates Through(ModuleHarness &h, Module *m, const std::vector<bess::PacketHandle> &packets) {
  return h.RunPackets(m, 0, packets, m, {0, 1});
}

size_t Out(Gates &g) { return g[0].size() + g[1].size(); }

// A wrong TCP or UDP checksum is replaced by the reference, untagged, under
// an 802.1Q tag and under 802.1ad + 802.1Q, with and without IP options.
// The old code never emitted a TCP packet, and passed tagged frames
// unchanged.
TEST(L4ChecksumTest, RecomputesTheChecksum) {
  ModuleHarness h;
  Module *m = Create(h, false);
  ASSERT_NE(nullptr, m);
  for (bool tcp : {false, true}) {
    for (const Shape &s : kShapes) {
      Gates out = Through(h, m, {MakePacket(Frame(tcp, s, kWrong))});
      EXPECT_EQ(std::vector<Bytes>{Frame(tcp, s)}, out[0]) << Name(tcp, s);
    }
  }
}

// Verify mode: right on gate 0, wrong on gate 1; a UDP checksum of 0 is "no
// checksum" (RFC 768) and passes. The old code passed tagged frames
// unverified.
TEST(L4ChecksumTest, VerifiesTheChecksum) {
  ModuleHarness h;
  Module *m = Create(h, true);
  ASSERT_NE(nullptr, m);
  for (bool tcp : {false, true}) {
    for (const Shape &s : kShapes) {
      Gates out = Through(h, m, {MakePacket(Frame(tcp, s)), MakePacket(Frame(tcp, s, kWrong))});
      EXPECT_EQ(std::vector<Bytes>{Frame(tcp, s)}, out[0]) << Name(tcp, s);
      EXPECT_EQ(std::vector<Bytes>{Frame(tcp, s, kWrong)}, out[1]) << Name(tcp, s);
    }
  }
  for (const Shape &s : kShapes) {
    Gates out = Through(h, m, {MakePacket(Frame(false, s, 0))});
    EXPECT_EQ(std::vector<Bytes>{Frame(false, s, 0)}, out[0]) << Name(false, s);
  }
  // TCP has no "no checksum".
  Gates out = Through(h, m, {MakePacket(Frame(true, 0, 0))});
  EXPECT_EQ(std::vector<Bytes>{Frame(true, 0, 0)}, out[1]);
}

// A right checksum stored as -0 (0xffff where 0x0000 is computed) passes
// verify, in one segment and in a chain; recomputing writes 0x0000 for TCP
// and 0xffff (unchanged) for UDP.
TEST(L4ChecksumTest, AMinusZeroChecksumIsRight) {
  ModuleHarness h;
  Module *verify = Create(h, true);
  Module *fix = Create(h, false);
  ASSERT_NE(nullptr, verify);
  ASSERT_NE(nullptr, fix);
  for (bool tcp : {false, true}) {
    for (const Shape &s : kShapes) {
      const size_t l4 = L4(s);
      // TCP: the window word; UDP: the source port.
      const Bytes minus = MinusZero(Frame(tcp, s), l4 + (tcp ? 14 : 0), l4 + (tcp ? 16 : 6));
      Bytes want = minus;
      if (tcp) {
        Set16(want, l4 + 16, 0);
      }
      for (size_t split : {size_t{0}, l4 + 25}) {
        Gates out = Through(h, verify, {MakePacket(minus, split)});
        EXPECT_EQ(std::vector<Bytes>{minus}, out[0]) << Name(tcp, s) << " split " << split;
        out = Through(h, fix, {MakePacket(minus, split)});
        EXPECT_EQ(std::vector<Bytes>{want}, out[0]) << Name(tcp, s) << " split " << split;
      }
    }
  }
}

// A frame cut short anywhere (the Ethernet header, a tag, the IP or L4
// header, or the datagram the IP length states) is dropped in both modes,
// never summed or written past its data. The old code summed up to the UDP
// or IP length from the first segment and forwarded each UDP one.
TEST(L4ChecksumTest, AShortFrameIsDropped) {
  ModuleHarness h;
  for (bool verify : {false, true}) {
    Module *m = Create(h, verify);
    ASSERT_NE(nullptr, m);
    for (bool tcp : {false, true}) {
      for (const Shape &s : kShapes) {
        const Bytes full = Frame(tcp, s, kWrong);
        for (size_t len = 1; len < full.size(); len++) {
          const Bytes cut(full.begin(), full.begin() + static_cast<ptrdiff_t>(len));
          Gates out = Through(h, m, {MakePacket(cut)});
          EXPECT_EQ(0u, Out(out)) << "verify " << verify << " " << Name(tcp, s) << " len "
                                  << len;
        }
      }
    }
  }
}

// A chain whose first segment ends inside the IP or L4 header is dropped
// (D-121: the headers must be in the first segment). One split inside the
// payload is summed across both segments: the checksum is the reference,
// verify passes it and fails a wrong one, and a UDP 0 passes. The old code
// summed the first segment's buffer past its data.
TEST(L4ChecksumTest, AChainIsSummedAcrossItsSegments) {
  ModuleHarness h;
  Module *fix = Create(h, false);
  Module *verify = Create(h, true);
  ASSERT_NE(nullptr, fix);
  ASSERT_NE(nullptr, verify);
  for (bool tcp : {false, true}) {
    for (const Shape &s : kShapes) {
      const size_t l4 = L4(s);
      const size_t l4_header = tcp ? 20 : 8;
      const Bytes in = Frame(tcp, s, kWrong);
      for (size_t split = L3(s.tags) + 1; split < l4 + l4_header; split++) {
        Gates out = Through(h, fix, {MakePacket(in, split)});
        EXPECT_EQ(0u, Out(out)) << Name(tcp, s) << " split " << split;
      }
      for (size_t split = l4 + l4_header; split < in.size(); split += 5) {
        Gates out = Through(h, fix, {MakePacket(in, split)});
        EXPECT_EQ(std::vector<Bytes>{Frame(tcp, s)}, out[0]) << Name(tcp, s) << " split " << split;
        out = Through(h, verify, {MakePacket(Frame(tcp, s), split), MakePacket(in, split)});
        EXPECT_EQ(std::vector<Bytes>{Frame(tcp, s)}, out[0])
            << "verify " << Name(tcp, s) << " split " << split;
        EXPECT_EQ(std::vector<Bytes>{in}, out[1])
            << "verify " << Name(tcp, s) << " split " << split;
        if (!tcp) {
          out = Through(h, verify, {MakePacket(Frame(false, s, 0), split)});
          EXPECT_EQ(std::vector<Bytes>{Frame(false, s, 0)}, out[0])
              << "verify UDP 0 " << Name(tcp, s) << " split " << split;
        }
      }
    }
  }
}

// A clone's payload is copied before the write: the other holder keeps its
// bytes. The old code wrote the shared bytes.
TEST(L4ChecksumTest, ASharedFrameIsCopiedBeforeTheWrite) {
  ModuleHarness h;
  Module *m = Create(h, false);
  ASSERT_NE(nullptr, m);
  for (bool tcp : {false, true}) {
    const Bytes in = Frame(tcp, 0, kWrong);
    for (size_t split : {0u, 60u}) {
      bess::PacketHandle original = MakePacket(in, split);
      ASSERT_NE(nullptr, original);
      bess::PacketHandle clone = bess::PacketClone(original);
      ASSERT_NE(nullptr, clone);
      Gates out = Through(h, m, {clone});
      EXPECT_EQ(std::vector<Bytes>{Frame(tcp, 0)}, out[0]) << "tcp " << tcp << " split " << split;
      EXPECT_EQ(in, PacketBytes(original))
          << "tcp " << tcp << " split " << split << ": the original is untouched";
      bess::PacketFree(original);
    }
  }
}

// IP and UDP lengths that lie, larger or smaller than the frame, are
// dropped in both modes. The old code summed over the UDP length (or, for
// TCP, the IP length) whatever it was.
TEST(L4ChecksumTest, ALyingLengthIsDropped) {
  ModuleHarness h;
  for (bool verify : {false, true}) {
    Module *m = Create(h, verify);
    ASSERT_NE(nullptr, m);
    for (bool tcp : {false, true}) {
      const Bytes good = Frame(tcp, 0, kWrong);
      const size_t ip_len = good.size() - 14;
      for (size_t lie : {ip_len + 1, ip_len + 1000, size_t{19}, size_t{39}}) {
        Bytes in = good;
        Set16(in, 16, lie);
        Gates out = Through(h, m, {MakePacket(in)});
        EXPECT_EQ(0u, Out(out)) << "verify " << verify << " tcp " << tcp << " IP length " << lie;
      }
    }
    // UDP: a length past the IP payload, or under the UDP header.
    const Bytes udp = Frame(false, 0, kWrong);
    const size_t udp_len = udp.size() - 34;
    for (size_t lie : {udp_len + 1, size_t{0xffff}, size_t{7}, size_t{0}}) {
      Bytes in = udp;
      Set16(in, 38, lie);
      Gates out = Through(h, m, {MakePacket(in)});
      EXPECT_EQ(0u, Out(out)) << "verify " << verify << " UDP length " << lie;
    }
  }
}

// A UDP length shorter than the IP payload is the datagram (RFC 768; the
// rest is trailing data, as Linux reads it): its checksum covers the UDP
// length, in one segment and in a chain that splits only the trailing data.
// A chain that splits the datagram itself passes unchanged on gate 0 in both
// modes: packet_checksum.h sums UDP only over the whole IP payload.
TEST(L4ChecksumTest, AUdpLengthShorterThanTheIpPayloadIsTheDatagram) {
  ModuleHarness h;
  Module *fix = Create(h, false);
  Module *verify = Create(h, true);
  ASSERT_NE(nullptr, fix);
  ASSERT_NE(nullptr, verify);
  const Bytes udp = Frame(false, 0, kWrong);
  const size_t udp_len = udp.size() - 34;
  for (size_t len : {udp_len - 1, udp_len - 10, size_t{8}}) {
    Bytes in = udp;
    Set16(in, 38, len);
    // The reference: the builder's checksum of the datagram alone.
    Bytes want = in;
    Set16(want, 40, Get16(Frame(false, 0, std::nullopt, kPayload.substr(0, len - 8)), 40));
    Gates out = Through(h, fix, {MakePacket(in)});
    EXPECT_EQ(std::vector<Bytes>{want}, out[0]) << "UDP length " << len;
    out = Through(h, verify, {MakePacket(want), MakePacket(in)});
    EXPECT_EQ(std::vector<Bytes>{want}, out[0]) << "UDP length " << len;
    EXPECT_EQ(std::vector<Bytes>{in}, out[1]) << "UDP length " << len;
    for (size_t split = 42; split < in.size(); split++) {
      const bool whole = split >= 34 + len;
      out = Through(h, fix, {MakePacket(in, split)});
      EXPECT_EQ(std::vector<Bytes>{whole ? want : in}, out[0])
          << "UDP length " << len << " split " << split;
      out = Through(h, verify, {MakePacket(in, split)});
      EXPECT_EQ(std::vector<Bytes>{in}, out[whole ? 1 : 0])
          << "UDP length " << len << " split " << split;
    }
  }
}

// Bytes past the IP length (Ethernet padding) are not summed: a TCP frame
// whose IP length stops short of the frame gets the checksum of the datagram
// the IP header states. The old code never emitted it.
TEST(L4ChecksumTest, TrailingBytesPastTheIpLengthAreNotSummed) {
  ModuleHarness h;
  Module *m = Create(h, false);
  ASSERT_NE(nullptr, m);
  const std::string_view head = kPayload.substr(0, 20);
  const Bytes tail = {'p', 'a', 'd', 'd', 'i', 'n', 'g', '!', '!', '!'};
  for (bool tcp : {false, true}) {
    Bytes in = Frame(tcp, 0, kWrong, head);
    in.insert(in.end(), tail.begin(), tail.end());
    Bytes want = Frame(tcp, 0, std::nullopt, head);
    want.insert(want.end(), tail.begin(), tail.end());
    Gates out = Through(h, m, {MakePacket(in)});
    EXPECT_EQ(std::vector<Bytes>{want}, out[0]) << "tcp " << tcp;
  }
}

// Every packet leaves on a gate or is dropped as malformed, in both modes,
// and what has no transport checksum to fix passes unchanged: ICMP, ARP,
// IPv6, a non-first fragment (no L4 header: its payload is never summed or
// written) and a first fragment (not the whole datagram). The old code never
// emitted ICMP (nor TCP without verify), and rewrote the fragments' bytes
// 6-7 of the IP payload as a UDP checksum.
TEST(L4ChecksumTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "02:00:00:00:00:01", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                        .PadTo(60)
                        .Build();
  const Bytes v6 = PacketBuilder()
                       .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                       .IPv6("2001:db8::1", "2001:db8::2")
                       .Udp(1000, 2000)
                       .PadTo(80)
                       .Build();
  const Bytes icmp = PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .IPv4("10.0.0.1", "10.0.0.2")
                         .Icmp()
                         .Payload(kPayload)
                         .Build();
  const Bytes later = Frame(false, 0, kWrong, kPayload, 0x0010);
  const Bytes first = Frame(false, 1, kWrong, kPayload, 0x2000);
  const Bytes tcp_later = Frame(true, 2, kWrong, kPayload, 0x0020);
  const Bytes udp = Frame(false, 0);
  const Bytes tcp = Frame(true, 0, kWrong);
  const Bytes runt(udp.begin(), udp.begin() + 40);
  for (bool verify : {false, true}) {
    Module *m = Create(h, verify);
    ASSERT_NE(nullptr, m);
    Gates out = Through(h, m,
                        {MakePacket(udp), MakePacket(tcp), MakePacket(icmp), MakePacket(arp),
                         MakePacket(v6), MakePacket(later), MakePacket(first),
                         MakePacket(tcp_later), MakePacket(runt)});
    if (verify) {
      EXPECT_EQ((std::vector<Bytes>{udp, icmp, arp, v6, later, first, tcp_later}), out[0]);
      EXPECT_EQ(std::vector<Bytes>{tcp}, out[1]);
    } else {
      EXPECT_EQ((std::vector<Bytes>{udp, Frame(true, 0), icmp, arp, v6, later, first, tcp_later}),
                out[0]);
      EXPECT_TRUE(out[1].empty());
    }
    EXPECT_EQ(8u, Out(out)) << "verify " << verify << ": 9 in, the runt dropped";
  }
}

}  // namespace
