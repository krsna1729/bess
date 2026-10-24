// SPDX-License-Identifier: BSD-3-Clause

// The NAT module in process (was bessctl/module_tests/nat.py): its arguments,
// gates (igate 0 -> ogate 1 forward, igate 1 -> ogate 0 reverse), growth
// through the control side's requests, usage commands and initial argument.
// The translation library itself is tested in core/nat/; usage commands while
// a worker runs are tests/live/nat_test.cc.

#include <gtest/gtest.h>
#include <google/protobuf/util/message_differencer.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "module.h"
#include "packet.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::PacketBytes;

constexpr char kInside[] = "172.16.0.2";
constexpr char kRemote[] = "8.8.8.8";
constexpr char kPublic[] = "192.168.1.1";

enum class L4 { kUdp, kUdpNoChecksum, kTcp, kIcmp };

// Ether / IPv4 / L4 / "helloworld". `sport` is the ICMP echo id (`dport`
// unused); a UDP without checksum carries 0.
Bytes Frame(L4 l4, const char *src, const char *dst, uint16_t sport, uint16_t dport) {
  PacketBuilder b;
  b.Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae").IPv4(src, dst);
  switch (l4) {
    case L4::kUdp:
      b.Udp(sport, dport);
      break;
    case L4::kUdpNoChecksum:
      b.Udp(sport, dport);
      b.Last<PacketBuilder::UdpHdr>().checksum = 0;
      break;
    case L4::kTcp:
      b.Tcp(sport, dport);
      break;
    case L4::kIcmp:
      b.Add(PacketBuilder::IcmpHdr{.type = 8, .code = 0, .id = sport, .seq = 0});
      break;
  }
  return b.Payload("helloworld").Build();
}

uint16_t Get16(const Bytes &b, size_t at) {
  return static_cast<uint16_t>(b[at] << 8 | b[at + 1]);
}

bess::pb::NATArg Arg(uint64_t capacity = 0, uint64_t max_capacity = 0, bool shared = false,
                     bool usage = false) {
  bess::pb::NATArg arg;
  arg.add_ext_addrs()->set_ext_addr(kPublic);
  arg.set_capacity(capacity);
  arg.set_max_capacity(max_capacity);
  arg.set_shared(shared);
  arg.set_usage(usage);
  return arg;
}

// Four packets: the original goes out translated to the external address
// (the NAT picks the source port or ICMP id: read from its output); the
// reply to that comes back translated to the original endpoints. Bytes
// equal, checksums included (a zero UDP checksum stays zero).
void ExpectTranslatesBothWays(ModuleHarness &h, Module *nat, L4 l4) {
  const uint16_t sport = l4 == L4::kTcp ? 52428 : l4 == L4::kIcmp ? 0 : 56797;
  const uint16_t dport = l4 == L4::kTcp ? 80 : l4 == L4::kIcmp ? 0 : 53;

  auto out = h.Run(nat, 0, {Frame(l4, kInside, kRemote, sport, dport)}, {0, 1});
  EXPECT_TRUE(out[0].empty());
  ASSERT_EQ(1u, out[1].size());
  const Bytes &natted = out[1][0];
  // The source port (TCP/UDP) and the echo id both sit 34 bytes in.
  ASSERT_GE(natted.size(), 42u);
  const uint16_t mapped = Get16(natted, l4 == L4::kIcmp ? 38 : 34);
  EXPECT_EQ(Frame(l4, kPublic, kRemote, mapped, dport), natted);

  // The reply swaps the ports (ICMP: the same echo, the mapped id).
  const Bytes reply = l4 == L4::kIcmp ? Frame(l4, kRemote, kPublic, mapped, 0)
                                      : Frame(l4, kRemote, kPublic, dport, mapped);
  out = h.Run(nat, 1, {reply}, {0, 1});
  EXPECT_TRUE(out[1].empty());
  ASSERT_EQ(1u, out[0].size());
  const Bytes back = l4 == L4::kIcmp ? Frame(l4, kRemote, kInside, sport, 0)
                                     : Frame(l4, kRemote, kInside, dport, sport);
  EXPECT_EQ(back, out[0][0]);
}

TEST(NatModuleTest, TranslatesUdp) {
  ModuleHarness h;
  Module *nat = h.Create("NAT", Arg());
  ASSERT_NE(nullptr, nat);
  ExpectTranslatesBothWays(h, nat, L4::kUdp);
}

TEST(NatModuleTest, TranslatesUdpWithZeroChecksum) {
  ModuleHarness h;
  Module *nat = h.Create("NAT", Arg());
  ASSERT_NE(nullptr, nat);
  ExpectTranslatesBothWays(h, nat, L4::kUdpNoChecksum);
}

TEST(NatModuleTest, TranslatesTcp) {
  ModuleHarness h;
  Module *nat = h.Create("NAT", Arg());
  ASSERT_NE(nullptr, nat);
  ExpectTranslatesBothWays(h, nat, L4::kTcp);
}

TEST(NatModuleTest, TranslatesIcmp) {
  ModuleHarness h;
  Module *nat = h.Create("NAT", Arg());
  ASSERT_NE(nullptr, nat);
  ExpectTranslatesBothWays(h, nat, L4::kIcmp);
}

// One NAT for every worker (TP6): the same translations.
TEST(NatModuleTest, SharedTranslatesTheSame) {
  for (const L4 l4 : {L4::kUdp, L4::kTcp, L4::kIcmp}) {
    SCOPED_TRACE(static_cast<int>(l4));
    ModuleHarness h;
    Module *nat = h.Create("NAT", Arg(1024, 0, /*shared=*/true));
    ASSERT_NE(nullptr, nat);
    ExpectTranslatesBothWays(h, nat, l4);
  }
}

// Owned (TP5) and shared (TP6): starts at 4 mappings, may grow to 64; at 3 of
// 4 it asks the control side (the daemon's maintenance loop; here the test
// delivers the requests) for a larger table and keeps translating. 40 flows
// all get a mapping only if the table grew, four times.
TEST(NatModuleTest, GrowsWithoutStopping) {
  for (const bool shared : {false, true}) {
    SCOPED_TRACE(shared ? "shared" : "owned");
    ModuleHarness h;
    Module *nat = h.Create("NAT", Arg(4, 64, shared));
    ASSERT_NE(nullptr, nat);
    int translated = 0;
    for (uint16_t flow = 0; flow < 40; flow++) {
      const Bytes pkt = PacketBuilder()
                            .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae")
                            .IPv4(kInside, kRemote)
                            .Udp(static_cast<uint16_t>(20000 + flow), 53)
                            .Payload("x")
                            .Build();
      // A packet that finds the table full is dropped: retry while the
      // control side allocates and the packet path migrates.
      for (int attempt = 0; attempt < 200; attempt++) {
        if (h.Run(nat, 0, {pkt}, {0, 1})[1].size() == 1) {
          translated++;
          break;
        }
        bess::runtime::runtime().requests().Deliver();
      }
    }
    EXPECT_EQ(40, translated);
    EXPECT_EQ("40 entries", nat->GetDesc());
  }
}

// Usage counters (TP7): an interim report gives each mapping's counts so far,
// owned and shared.
TEST(NatModuleTest, UsageReportGivesTheCountsSoFar) {
  for (const bool shared : {false, true}) {
    SCOPED_TRACE(shared ? "shared" : "owned");
    ModuleHarness h;
    Module *nat = h.Create("NAT", Arg(64, 0, shared, /*usage=*/true));
    ASSERT_NE(nullptr, nat);
    const Bytes out = PacketBuilder()
                          .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae")
                          .IPv4(kInside, kRemote)
                          .Udp(33000, 53)
                          .Payload(std::string(72, 'y'))  // 100 IP bytes
                          .Build();
    for (int i = 0; i < 3; i++) {
      ASSERT_EQ(1u, h.Run(nat, 0, {out}, {0, 1})[1].size());
    }
    ASSERT_EQ(0, h.Action(nat, "request_usage_report", bess::pb::EmptyArg()).error().code());
    // The report walks the table on the packet path: one more batch (a
    // packet that is dropped: an inbound one with no mapping).
    const Bytes stray = PacketBuilder()
                            .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae")
                            .IPv4(kRemote, kPublic)
                            .Udp(53, 9)
                            .Payload("z")
                            .Build();
    auto dropped = h.Run(nat, 1, {stray}, {0, 1});
    EXPECT_TRUE(dropped[0].empty());
    EXPECT_TRUE(dropped[1].empty());

    bess::pb::NATUsageResponse resp;
    CommandResponse drained = h.Action(nat, "drain_usage", bess::pb::EmptyArg());
    ASSERT_EQ(0, drained.error().code()) << drained.error().errmsg();
    ASSERT_TRUE(drained.data().UnpackTo(&resp));
    EXPECT_EQ(1u, resp.reports_done());
    ASSERT_EQ(1, resp.records_size());
    const auto &record = resp.records(0);
    EXPECT_FALSE(record.final());
    EXPECT_EQ(kInside, record.internal_addr());
    EXPECT_EQ(33000u, record.internal_port());
    EXPECT_EQ(3u, record.packets());
    EXPECT_EQ(300u, record.bytes());
  }
}

// D-129: the usage commands are actions (drain_usage answers a
// NATUsageResponse).
TEST(NatModuleTest, UsageCommandsAreActions) {
  ModuleHarness h;
  Module *nat = h.Create("NAT", Arg(64, 0, false, /*usage=*/true));
  ASSERT_NE(nullptr, nat);

  CommandResponse r = h.Action(nat, "request_usage_report", bess::pb::EmptyArg());
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  r = h.Action(nat, "drain_usage", bess::pb::EmptyArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  bess::pb::NATUsageResponse resp;
  ASSERT_TRUE(r.data().UnpackTo(&resp));
  EXPECT_EQ(0, resp.records_size());
}

// The ports (ICMP: the echo id) of `l4`'s outbound test flow, and its frame
// from kInside to kRemote.
uint16_t SourcePort(L4 l4) { return l4 == L4::kTcp ? 52428 : l4 == L4::kIcmp ? 7 : 56797; }
uint16_t DestPort(L4 l4) { return l4 == L4::kTcp ? 80 : l4 == L4::kIcmp ? 0 : 53; }
Bytes Outbound(L4 l4) { return Frame(l4, kInside, kRemote, SourcePort(l4), DestPort(l4)); }

// `natted` is `l4`'s outbound frame translated: the external address and the
// port (or echo id) the NAT picked, checksums included.
void ExpectTranslated(L4 l4, const Bytes &natted) {
  ASSERT_GE(natted.size(), 42u);
  const uint16_t mapped = Get16(natted, l4 == L4::kIcmp ? 38 : 34);
  EXPECT_EQ(Frame(l4, kPublic, kRemote, mapped, DestPort(l4)), natted);
}

// The rewrite is in place: a clone's payload (or the source a clone shares)
// is copied first, in one segment and in a chain split after the headers,
// and the other holder keeps its frame. The old code translated the shared
// bytes.
TEST(NatModuleTest, ASharedFrameIsCopiedBeforeTheTranslation) {
  for (const L4 l4 : {L4::kUdp, L4::kTcp, L4::kIcmp}) {
    ModuleHarness h;
    Module *nat = h.Create("NAT", Arg());
    ASSERT_NE(nullptr, nat);
    const Bytes frame = Outbound(l4);
    for (bool send_clone : {true, false}) {
      for (size_t split : {size_t{0}, frame.size() - 5}) {
        SCOPED_TRACE(::testing::Message() << "l4 " << static_cast<int>(l4) << ", clone "
                                          << send_clone << ", split " << split);
        bess::PacketHandle original = MakePacket(frame, split);
        ASSERT_NE(nullptr, original);
        bess::PacketHandle clone = bess::PacketClone(original);
        ASSERT_NE(nullptr, clone);
        auto out = h.RunPackets(nat, 0, {send_clone ? clone : original}, nat, {0, 1});
        bess::PacketHandle kept = send_clone ? original : clone;
        EXPECT_TRUE(out[0].empty());
        ASSERT_EQ(1u, out[1].size());
        ExpectTranslated(l4, out[1][0]);
        EXPECT_EQ(frame, PacketBytes(kept)) << "the other holder's frame is untouched";
        bess::PacketFree(kept);
      }
    }
  }
}

// Every packet leaves translated on gate 1 or is dropped: UDP in one
// segment, TCP in a chain split after its headers, an ICMP echo in a clone;
// a runt, a chain split inside the IPv4 header, one split inside the TCP
// header and an ARP frame are dropped.
TEST(NatModuleTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  Module *nat = h.Create("NAT", Arg());
  ASSERT_NE(nullptr, nat);
  const Bytes udp = Outbound(L4::kUdp);
  const Bytes tcp = Outbound(L4::kTcp);
  const Bytes icmp = Outbound(L4::kIcmp);
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "02:00:00:00:00:01", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                        .PadTo(60)
                        .Build();
  bess::PacketHandle source = MakePacket(icmp);
  ASSERT_NE(nullptr, source);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(nullptr, clone);
  auto out = h.RunPackets(nat, 0,
                          {MakePacket(udp), MakePacket(tcp, 54), clone,
                           MakePacket(Bytes(udp.begin(), udp.begin() + 30)), MakePacket(udp, 30),
                           MakePacket(tcp, 44), MakePacket(arp)},
                          nat, {0, 1});
  EXPECT_TRUE(out[0].empty());
  ASSERT_EQ(3u, out[1].size()) << "7 in, 4 dropped";
  ExpectTranslated(L4::kUdp, out[1][0]);
  ExpectTranslated(L4::kTcp, out[1][1]);
  ExpectTranslated(L4::kIcmp, out[1][2]);
  EXPECT_EQ(icmp, PacketBytes(source));
  bess::PacketFree(source);
}

}  // namespace
