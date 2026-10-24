// SPDX-License-Identifier: BSD-3-Clause

// IPChecksum in process (was bessctl/module_tests/ip_checksum.py): the IPv4
// header checksum is recomputed (or verified) where the checked parse finds
// the header; a frame that is not IPv4 passes unchanged, a frame whose
// headers do not fit its first segment or lie about their lengths is dropped,
// and a shared payload is copied first (D-153).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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

// eth / `tags` VLAN tags (0, 802.1Q, 802.1ad + 802.1Q) / IP(1.2.3.4 ->
// 2.3.4.5, ttl 98, `frag`, `options` bytes of NOP options) / UDP / payload;
// the builder computes the right IP checksum (the reference), zeroed when
// `wrong`.
Bytes Packet(int tags, bool wrong, uint16_t frag = 0, size_t options = 0) {
  PacketBuilder b;
  b.Ether("12:34:de:ad:be:ef", "de:ad:be:ef:12:34");
  if (tags == 1) {
    b.Dot1Q(6);
  } else if (tags == 2) {
    b.Dot1AD(5).Dot1Q(6);
  }
  b.IPv4("1.2.3.4", "2.3.4.5");
  b.Last<PacketBuilder::IPv4Hdr>().ttl = 98;
  b.Last<PacketBuilder::IPv4Hdr>().frag = frag;
  b.Last<PacketBuilder::IPv4Hdr>().options = Bytes(options, 0x01);
  if (wrong) {
    b.Last<PacketBuilder::IPv4Hdr>().checksum = 0x0000;
  }
  return b.Udp(10001, 10002).Payload("helloworldhelloworldhelloworld").Build();
}

// A frame's header shape: VLAN tags and IPv4 option bytes (IHL 5, 6 or 7).
struct Shape {
  int tags;
  size_t options;
};
constexpr Shape kShapes[] = {{0, 0}, {1, 0}, {2, 0}, {0, 8}, {2, 4}};

Bytes Packet(const Shape &s, bool wrong) { return Packet(s.tags, wrong, 0, s.options); }

size_t L3(int tags) { return 14 + 4 * static_cast<size_t>(tags); }

uint16_t Get16(const Bytes &b, size_t offset) {
  return static_cast<uint16_t>(unsigned{b[offset]} << 8u | b[offset + 1]);
}

void Set16(Bytes &b, size_t offset, size_t value) {
  b[offset] = static_cast<uint8_t>(value >> 8u);
  b[offset + 1] = static_cast<uint8_t>(value & 0xffu);
}

// `frame`, its right checksum at `field` made 0x0000 by adding it (one's
// complement) to the summed word at `word`, then stored as 0xffff: -0, as
// an incremental update may leave it (RFC 1624), and as right as 0x0000.
Bytes MinusZero(Bytes frame, size_t word, size_t field) {
  uint32_t sum = uint32_t{Get16(frame, word)} + Get16(frame, field);
  sum = (sum & 0xffffu) + (sum >> 16u);
  Set16(frame, word, sum);
  Set16(frame, field, 0xffff);
  return frame;
}

Module *Create(ModuleHarness &h, bool verify) {
  bess::pb::IPChecksumArg arg;
  arg.set_verify(verify);
  return h.Create("IPChecksum", arg);
}

// `packets` through `m`: what left on gates 0 and 1.
Gates Through(ModuleHarness &h, Module *m, const std::vector<bess::PacketHandle> &packets) {
  return h.RunPackets(m, 0, packets, m, {0, 1});
}

size_t Out(Gates &g) { return g[0].size() + g[1].size(); }

// A zero IP checksum is replaced by the right one, untagged, under an
// 802.1Q tag and under 802.1ad + 802.1Q, with and without options.
TEST(IPChecksumTest, RecomputesTheIpChecksum) {
  ModuleHarness h;
  Module *ipc = Create(h, false);
  ASSERT_NE(nullptr, ipc);
  for (const Shape &s : kShapes) {
    const Bytes in = Packet(s, true);
    const Bytes want = Packet(s, false);
    ASSERT_NE(in, want) << s.tags << "/" << s.options;
    auto out = h.Run(ipc, 0, {in}, {0});
    ASSERT_EQ(1u, out[0].size()) << s.tags << "/" << s.options;
    EXPECT_EQ(want, out[0][0]) << s.tags << "/" << s.options;
  }
}

// Verify mode: a right checksum leaves on gate 0, a wrong one on gate 1, at
// every tag depth, with and without options.
TEST(IPChecksumTest, VerifiesTheIpChecksum) {
  ModuleHarness h;
  Module *ipc = Create(h, true);
  ASSERT_NE(nullptr, ipc);
  for (const Shape &s : kShapes) {
    auto out = h.Run(ipc, 0, {Packet(s, false), Packet(s, true)}, {0, 1});
    EXPECT_EQ(std::vector<Bytes>{Packet(s, false)}, out[0]) << s.tags << "/" << s.options;
    EXPECT_EQ(std::vector<Bytes>{Packet(s, true)}, out[1]) << s.tags << "/" << s.options;
  }
}

// A right checksum stored as -0 (0xffff where 0x0000 is computed) passes
// verify, in one segment and in a chain; recomputing writes 0x0000.
TEST(IPChecksumTest, AMinusZeroChecksumIsRight) {
  ModuleHarness h;
  Module *verify = Create(h, true);
  Module *fix = Create(h, false);
  ASSERT_NE(nullptr, verify);
  ASSERT_NE(nullptr, fix);
  for (const Shape &s : kShapes) {
    const size_t l3 = L3(s.tags);
    const Bytes minus = MinusZero(Packet(s, false), l3 + 4, l3 + 10);  // the IP id
    Bytes plus = minus;
    Set16(plus, l3 + 10, 0);
    for (size_t split : {size_t{0}, l3 + 20 + s.options + 10}) {
      Gates out = Through(h, verify, {MakePacket(minus, split), MakePacket(plus, split)});
      EXPECT_EQ((std::vector<Bytes>{minus, plus}), out[0])
          << s.tags << "/" << s.options << " split " << split;
      out = Through(h, fix, {MakePacket(minus, split)});
      EXPECT_EQ(std::vector<Bytes>{plus}, out[0])
          << s.tags << "/" << s.options << " split " << split;
    }
  }
}

// A frame cut short anywhere (inside the Ethernet header, a tag, the IP
// header or the datagram the IP length states) is dropped in both modes,
// never read or written past its data. The old code read and wrote the
// header at fixed offsets and forwarded every one.
TEST(IPChecksumTest, AShortFrameIsDropped) {
  ModuleHarness h;
  for (bool verify : {false, true}) {
    Module *ipc = Create(h, verify);
    ASSERT_NE(nullptr, ipc);
    for (const Shape &s : kShapes) {
      const Bytes full = Packet(s, true);
      for (size_t len = 1; len < full.size(); len++) {
        const Bytes cut(full.begin(), full.begin() + static_cast<ptrdiff_t>(len));
        Gates out = Through(h, ipc, {MakePacket(cut)});
        EXPECT_EQ(0u, Out(out)) << "verify " << verify << " shape " << s.tags << "/"
                                << s.options << " len " << len;
      }
    }
  }
}

// A chain whose first segment ends inside the IP (or UDP) header is dropped
// (D-121: the headers must be in the first segment); one split inside the
// payload is recomputed. The old code read the header past the first
// segment's data and forwarded the frame.
TEST(IPChecksumTest, AChainSplitInsideTheHeadersIsDropped) {
  ModuleHarness h;
  Module *ipc = Create(h, false);
  ASSERT_NE(nullptr, ipc);
  for (const Shape &s : kShapes) {
    const size_t l3 = L3(s.tags);
    const size_t headers = 20 + s.options + 8;  // IP and UDP
    const Bytes in = Packet(s, true);
    for (size_t split = l3 + 1; split < l3 + headers; split++) {
      Gates out = Through(h, ipc, {MakePacket(in, split)});
      EXPECT_EQ(0u, Out(out)) << "shape " << s.tags << "/" << s.options << " split " << split;
    }
    for (size_t split : {l3 + headers, l3 + headers + 7}) {
      Gates out = Through(h, ipc, {MakePacket(in, split)});
      EXPECT_EQ(std::vector<Bytes>{Packet(s, false)}, out[0])
          << "shape " << s.tags << "/" << s.options << " split " << split;
    }
  }
}

// A clone's payload is copied before the write: the other holder keeps its
// bytes. The old code wrote the shared bytes.
TEST(IPChecksumTest, ASharedFrameIsCopiedBeforeTheWrite) {
  ModuleHarness h;
  Module *ipc = Create(h, false);
  ASSERT_NE(nullptr, ipc);
  const Bytes in = Packet(0, true);
  for (size_t split : {0u, 50u}) {
    bess::PacketHandle original = MakePacket(in, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    Gates out = Through(h, ipc, {clone});
    EXPECT_EQ(std::vector<Bytes>{Packet(0, false)}, out[0]) << "split " << split;
    EXPECT_EQ(in, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

// An IP total length larger than the frame, smaller than the IP header, or
// smaller than the UDP datagram is a lie: the frame is dropped. A frame with
// trailing bytes past an honest IP length (Ethernet padding) is recomputed
// over the header alone. The old code trusted none of the lengths, and
// forwarded every one.
TEST(IPChecksumTest, ALyingIpLengthIsDropped) {
  ModuleHarness h;
  Module *ipc = Create(h, false);
  ASSERT_NE(nullptr, ipc);
  const Bytes good = Packet(0, true);
  const size_t ip_len = good.size() - 14;
  for (size_t lie : {ip_len + 1, ip_len + 1000, size_t{19}, size_t{20}, size_t{27}, ip_len - 1}) {
    Bytes in = good;
    in[16] = static_cast<uint8_t>(lie >> 8u);
    in[17] = static_cast<uint8_t>(lie & 0xffu);
    Gates out = Through(h, ipc, {MakePacket(in)});
    EXPECT_EQ(0u, Out(out)) << "IP length " << lie;
  }
  Bytes padded = good;
  padded.resize(good.size() + 20, 0);
  Bytes want = Packet(0, false);
  want.resize(good.size() + 20, 0);
  Gates out = Through(h, ipc, {MakePacket(padded)});
  EXPECT_EQ(std::vector<Bytes>{want}, out[0]);
}

// Every packet leaves on a gate or is dropped as malformed, in both modes:
// IPv4 right and wrong, a non-first fragment (its own header checksum is
// checked), ARP and IPv6 (unchanged), and a malformed frame (dropped). The
// old code forwarded the malformed one too.
TEST(IPChecksumTest, EveryPacketIsAccountedFor) {
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
  const Bytes good = Packet(0, false);
  const Bytes bad = Packet(1, true);
  const Bytes frag_good = Packet(2, false, 0x0010);
  const Bytes frag_bad = Packet(0, true, 0x0010);
  const Bytes runt(good.begin(), good.begin() + 30);
  for (bool verify : {false, true}) {
    Module *ipc = Create(h, verify);
    ASSERT_NE(nullptr, ipc);
    Gates out = Through(h, ipc,
                        {MakePacket(good), MakePacket(bad), MakePacket(arp), MakePacket(v6),
                         MakePacket(frag_good), MakePacket(frag_bad), MakePacket(runt)});
    if (verify) {
      EXPECT_EQ((std::vector<Bytes>{good, arp, v6, frag_good}), out[0]);
      EXPECT_EQ((std::vector<Bytes>{bad, frag_bad}), out[1]);
    } else {
      EXPECT_EQ((std::vector<Bytes>{good, Packet(1, false), arp, v6, frag_good,
                                    Packet(0, false, 0x0010)}),
                out[0]);
      EXPECT_TRUE(out[1].empty());
    }
    EXPECT_EQ(6u, Out(out)) << "verify " << verify << ": 7 in, the runt dropped";
  }
}

}  // namespace
