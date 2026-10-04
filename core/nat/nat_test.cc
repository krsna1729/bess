// SPDX-License-Identifier: BSD-3-Clause

// NAT library (M18, D-068): rewrite, port spans and allocation, bindings,
// expiry, and a differential test against the legacy module's algorithm.

#include "nat/nat.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "utils/checksum.h"
#include "utils/icmp.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"

#include "testing/allocation_faults.h"

namespace bess::nat {
namespace {

using conntrack::ParsedFlowPacket;
using conntrack::ParseFrame;
using conntrack::ParseStatus;

constexpr uint64_t kSec = 1'000'000'000;

void Put16(std::vector<uint8_t> &f, size_t off, uint16_t v) {
  f[off] = static_cast<uint8_t>(v >> 8);
  f[off + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t> &f, size_t off, uint32_t v) {
  Put16(f, off, static_cast<uint16_t>(v >> 16));
  Put16(f, off + 2, static_cast<uint16_t>(v));
}

// An Ethernet + IPv4 + L4 frame with valid checksums. For ICMP, `sport` is
// the identifier and `dport` the type.
std::vector<uint8_t> Frame(uint8_t proto, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                           bool udp_checksum = true) {
  const size_t l4len = proto == 6 ? 20 + 6 : proto == 17 ? 8 + 6 : 8 + 6;
  std::vector<uint8_t> f(14 + 20 + l4len, 0);
  Put16(f, 12, 0x0800);
  f[14] = 0x45;
  Put16(f, 16, static_cast<uint16_t>(20 + l4len));
  f[22] = 64;
  f[23] = proto;
  Put32(f, 26, src);
  Put32(f, 30, dst);
  for (size_t i = 0; i < 6; i++) f[f.size() - 6 + i] = static_cast<uint8_t>(0x30 + i);
  auto *ip = reinterpret_cast<utils::Ipv4 *>(f.data() + 14);
  uint8_t *l4 = f.data() + 34;
  if (proto == 6 || proto == 17) {
    Put16(f, 34, sport);
    Put16(f, 36, dport);
  }
  ip->checksum = utils::CalculateIpv4NoOptChecksum(*ip);
  if (proto == 6) {
    l4[12] = 5 << 4;
    auto *tcp = reinterpret_cast<utils::Tcp *>(l4);
    tcp->checksum = utils::CalculateIpv4TcpChecksum(*ip, *tcp);
  } else if (proto == 17) {
    Put16(f, 38, static_cast<uint16_t>(l4len));
    auto *udp = reinterpret_cast<utils::Udp *>(l4);
    if (udp_checksum) udp->checksum = utils::CalculateIpv4UdpChecksum(*ip, *udp);
  } else {
    l4[0] = static_cast<uint8_t>(dport);
    Put16(f, 38, sport);
    const uint16_t sum = utils::CalculateGenericChecksum(l4, l4len);
    std::memcpy(l4 + 2, &sum, 2);
  }
  return f;
}

bool ChecksumsValid(const std::vector<uint8_t> &f) {
  const auto *ip = reinterpret_cast<const utils::Ipv4 *>(f.data() + 14);
  const uint8_t *l4 = f.data() + 34;
  if (!utils::VerifyIpv4NoOptChecksum(*ip)) return false;
  if (ip->protocol == 6) return utils::VerifyIpv4TcpChecksum(*ip, *reinterpret_cast<const utils::Tcp *>(l4));
  if (ip->protocol == 17) {
    const auto *udp = reinterpret_cast<const utils::Udp *>(l4);
    return udp->checksum == 0 || utils::VerifyIpv4UdpChecksum(*ip, *udp);
  }
  return utils::VerifyGenericChecksum(l4, f.size() - 34);
}

Endpoint Src(const std::vector<uint8_t> &f) {
  ParsedFlowPacket p;
  EXPECT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  Verdict why;
  return Nat::EndpointOf(p, Direction::kForward, &why).value();
}

struct Harness {
  std::unique_ptr<Nat> nat;
  explicit Harness(std::vector<ExternalAddress> addrs, uint64_t timeout = 300 * kSec, size_t cap = 4096) {
    Nat::Config c;
    c.addresses = std::move(addrs);
    c.capacity = cap;
    c.timeout = timeout;
    c.granularity_shift = 0;
    c.seed = 7;
    nat = Nat::Create(c).value();
  }
  Verdict Send(std::vector<uint8_t> &f, Direction dir, uint64_t now) {
    ParsedFlowPacket p;
    if (ParseFrame(f, p) != ParseStatus::kOk) return Verdict::kUnsupported;
    return nat->Translate(f, p, dir, now);
  }
};

const uint32_t kInside = 0x0a000005, kRemote = 0x08080808, kPublic = 0xc6336401;
ExternalAddress Pub(uint32_t a = kPublic, std::vector<PortRange> r = {{0, 65535, false}}) {
  return {be32_t(a), std::move(r)};
}

TEST(NatTest, ForwardAndReverseRewriteKeepChecksumsValid) {
  Harness h({Pub()});
  for (const uint8_t proto : {6, 17, 1}) {
    auto out = Frame(proto, kInside, 40000, kRemote, proto == 1 ? 8 : 443);
    ASSERT_EQ(Verdict::kTranslated, h.Send(out, Direction::kForward, 0));
    EXPECT_TRUE(ChecksumsValid(out)) << int(proto);
    const Endpoint ext = Src(out);
    EXPECT_EQ(be32_t(kPublic), ext.addr);
    // The reply to the external endpoint comes back to the internal one.
    auto in = Frame(proto, kRemote, proto == 1 ? ext.port.value() : 443, kPublic,
                    proto == 1 ? 0 : ext.port.value());
    ASSERT_EQ(Verdict::kTranslated, h.Send(in, Direction::kReverse, 1)) << int(proto);
    EXPECT_TRUE(ChecksumsValid(in)) << int(proto);
    ParsedFlowPacket p;
    ASSERT_EQ(ParseStatus::kOk, ParseFrame(in, p));
    Verdict why;
    const Endpoint back = Nat::EndpointOf(p, Direction::kReverse, &why).value();
    EXPECT_EQ(be32_t(kInside), back.addr);
    EXPECT_EQ(40000, back.port.value());
  }
  // A UDP datagram without a checksum keeps none.
  auto bare = Frame(17, kInside, 40001, kRemote, 53, /*udp_checksum=*/false);
  ASSERT_EQ(Verdict::kTranslated, h.Send(bare, Direction::kForward, 0));
  EXPECT_EQ(0, bare[40] | bare[41]);
}

// An echo reply that is all zeros once translated (identifier 0, sequence 0,
// no payload) sums to +0, which only the checksum 0xffff verifies: the
// incremental update's 0x0000 is written as 0xffff (found by core/fuzz/nat_fuzz).
TEST(NatTest, IcmpRewriteToAnAllZeroMessageKeepsTheChecksumValid) {
  auto zero_reply = [](uint32_t src, uint32_t dst, uint16_t id) {
    auto f = Frame(1, src, id, dst, /*type=*/0);
    f.resize(34 + 8);  // no payload
    Put16(f, 16, 28);
    auto *ip = reinterpret_cast<utils::Ipv4 *>(f.data() + 14);
    ip->checksum = 0;
    ip->checksum = utils::CalculateIpv4NoOptChecksum(*ip);
    f[36] = f[37] = 0;
    const uint16_t sum = utils::CalculateGenericChecksum(f.data() + 34, 8);
    std::memcpy(f.data() + 36, &sum, 2);
    return f;
  };
  // Outbound: the only identifier is 0.
  Harness out({Pub(kPublic, {{0, 1, false}})});
  auto f = zero_reply(kInside, kRemote, 12);
  ASSERT_TRUE(ChecksumsValid(f));
  ASSERT_EQ(Verdict::kTranslated, out.Send(f, Direction::kForward, 0));
  EXPECT_EQ(0, f[38] | f[39]);
  EXPECT_TRUE(ChecksumsValid(f));
  EXPECT_EQ(0xffff, f[36] << 8 | f[37]);
  // Inbound: back to the internal identifier 0.
  Harness in({Pub(kPublic, {{5, 6, false}})});
  auto request = Frame(1, kInside, 0, kRemote, 8);
  ASSERT_EQ(Verdict::kTranslated, in.Send(request, Direction::kForward, 0));
  auto g = zero_reply(kRemote, kPublic, 5);
  ASSERT_EQ(Verdict::kTranslated, in.Send(g, Direction::kReverse, 1));
  EXPECT_EQ(0, g[38] | g[39]);
  EXPECT_TRUE(ChecksumsValid(g));
}

TEST(NatTest, EndpointIndependentMappingAndDirectionGuard) {
  Harness h({Pub()});
  auto a = Frame(17, kInside, 5000, kRemote, 53);
  auto b = Frame(17, kInside, 5000, 0x01010101, 123);
  h.Send(a, Direction::kForward, 0);
  h.Send(b, Direction::kForward, 0);
  EXPECT_EQ(Src(a), Src(b)) << "one mapping for one internal endpoint (EIM)";
  EXPECT_EQ(1u, h.nat->size());
  auto unknown = Frame(17, kRemote, 53, kPublic, 1);
  EXPECT_EQ(Verdict::kNoBinding, h.Send(unknown, Direction::kReverse, 0));
  // An outbound packet whose source equals a mapping's external endpoint is not
  // that mapping: refused, and the mapping is untouched. An inbound packet to a
  // mapping's internal endpoint is not a reply either.
  const Endpoint ext = Src(a);
  auto spoof = Frame(17, kPublic, ext.port.value(), kRemote, 53);
  EXPECT_EQ(Verdict::kConflict, h.Send(spoof, Direction::kForward, 0));
  auto to_inside = Frame(17, kRemote, 53, kInside, 5000);
  EXPECT_EQ(Verdict::kNoBinding, h.Send(to_inside, Direction::kReverse, 0));
  EXPECT_EQ(1u, h.nat->size());
  EXPECT_EQ(be32_t(kInside), h.nat->Find(ext)->internal.addr);
}

TEST(NatTest, PortClassesRangesAndRefusals) {
  // Privileged ports map to 1..1023 (never 0, 1023 included); others to 1024..end-1.
  EXPECT_EQ((std::pair<uint32_t, uint32_t>{1, 1024}), PortSpan({0, 65535}, 6, 80));
  EXPECT_EQ((std::pair<uint32_t, uint32_t>{1024, 65535}), PortSpan({0, 65535}, 17, 5000));
  EXPECT_EQ((std::pair<uint32_t, uint32_t>{0, 65535}), PortSpan({0, 65535}, 1, 0));
  EXPECT_EQ((std::pair<uint32_t, uint32_t>{2000, 3000}), PortSpan({2000, 3000}, 6, 5000));
  const auto none = PortSpan({2000, 3000}, 6, 80);
  EXPECT_GE(none.first, none.second) << "no privileged ports in 2000-2999";

  Harness h({Pub(kPublic, {{2000, 2004, false}, {5000, 5100, true}})});
  std::set<uint16_t> ports;
  for (uint16_t i = 0; i < 4; i++) {
    auto f = Frame(6, kInside, static_cast<uint16_t>(40000 + i), kRemote, 80);
    ASSERT_EQ(Verdict::kTranslated, h.Send(f, Direction::kForward, 0));
    ports.insert(Src(f).port.value());
  }
  EXPECT_EQ((std::set<uint16_t>{2000, 2001, 2002, 2003}), ports) << "end exclusive; suspended unused";
  auto f = Frame(6, kInside, 40010, kRemote, 80);
  EXPECT_EQ(Verdict::kExhausted, h.Send(f, Direction::kForward, 0));
  f = Frame(6, kInside, 22, kRemote, 80);
  EXPECT_EQ(Verdict::kExhausted, h.Send(f, Direction::kForward, 0)) << "no privileged span";
  f = Frame(17, kInside, 0, kRemote, 53);
  EXPECT_EQ(Verdict::kPortZero, h.Send(f, Direction::kForward, 0));
  f = Frame(1, kInside, 9, kRemote, 3);  // destination unreachable
  EXPECT_EQ(Verdict::kUnsupported, h.Send(f, Direction::kForward, 0));
  f = Frame(1, kInside, 9, kRemote, 14);  // timestamp reply: translated now (D-068)
  EXPECT_EQ(Verdict::kTranslated, h.Send(f, Direction::kForward, 0));
}

TEST(NatTest, PortPoolAllocatesEachPortOnceAndReuses) {
  PortPool pool(1);
  std::mt19937 rng(18);
  std::set<uint16_t> got;
  for (int i = 0; i < 300; i++) {
    const auto p = pool.Allocate(0, 17, 100, 400, rng());
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(got.insert(*p).second) << "port " << *p << " twice";
    ASSERT_GE(*p, 100);
    ASSERT_LT(*p, 400);
  }
  EXPECT_FALSE(pool.Allocate(0, 17, 100, 400, rng()).has_value());
  EXPECT_TRUE(pool.Allocate(0, 6, 100, 400, rng()).has_value()) << "TCP is another class";
  pool.Release(0, 17, 250);
  EXPECT_EQ(250, pool.Allocate(0, 17, 100, 400, rng()).value());
  // Spans that end mid-word and at 65536.
  PortPool edge(1);
  for (int i = 0; i < 3; i++) {
    const auto p = edge.Allocate(0, 6, 65533, 65536, rng());
    ASSERT_TRUE(p.has_value());
    EXPECT_GE(*p, 65533);
  }
  EXPECT_FALSE(edge.Allocate(0, 6, 65533, 65536, rng()).has_value());
}

TEST(NatTest, IdleMappingsExpireAndFreeTheirPorts) {
  Harness h({Pub(kPublic, {{3000, 3001, false}})}, /*timeout=*/100);
  auto a = Frame(17, kInside, 5000, kRemote, 53);
  ASSERT_EQ(Verdict::kTranslated, h.Send(a, Direction::kForward, 0));
  const Endpoint ext = Src(a);
  // Inbound packets do not refresh (RFC 4787 REQ-6).
  auto in = Frame(17, kRemote, 53, kPublic, ext.port.value());
  ASSERT_EQ(Verdict::kTranslated, h.Send(in, Direction::kReverse, 90));
  EXPECT_EQ(0u, h.nat->Expire(99, ~size_t{0}));
  EXPECT_EQ(1u, h.nat->Expire(101, ~size_t{0}));
  in = Frame(17, kRemote, 53, kPublic, ext.port.value());
  EXPECT_EQ(Verdict::kNoBinding, h.Send(in, Direction::kReverse, 102));
  // The single port is free again; an outbound refresh keeps a mapping alive.
  auto b = Frame(17, kInside, 6000, kRemote, 53);
  ASSERT_EQ(Verdict::kTranslated, h.Send(b, Direction::kForward, 200));
  auto b2 = Frame(17, kInside, 6000, kRemote, 53);
  ASSERT_EQ(Verdict::kTranslated, h.Send(b2, Direction::kForward, 280));
  EXPECT_EQ(0u, h.nat->Expire(350, ~size_t{0}));
  EXPECT_EQ(1u, h.nat->Expire(381, ~size_t{0}));
}

// Every allocation Create makes (the binding table's blocks and object, the
// wheel, the engine and its copies of the configuration), refused in turn:
// Create returns kOutOfMemory -- it does not throw -- and leaves nothing
// allocated.
// A fixed NAT refuses a maximum above its capacity: growth is GrowableNat's.
TEST(NatTest, FixedNatRefusesAMaximumAboveItsCapacity) {
  Nat::Config c;
  c.addresses = {Pub()};
  c.capacity = 64;
  c.max_capacity = 128;
  ASSERT_FALSE(Nat::Create(c).has_value());
  EXPECT_EQ(Nat::CreateError::kInvalidCapacity, Nat::Create(c).error());
}

TEST(NatFaultTest, CreateFailureAtEveryAllocationReturnsOutOfMemoryAndLeaksNothing) {
  Nat::Config c;
  c.addresses = {Pub(kPublic, {{1024, 4096, false}, {8000, 9000, false}}),
                 Pub(kPublic + 1, {{2000, 3000, false}})};
  c.capacity = 512;
  c.granularity_shift = 0;
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
    bool threw = false, injected = false;
    size_t allocations = 0, frees = 0;
    {
      const fault_injection::AllocationFaults faults(k);
      try {
        auto nat = Nat::Create(c);
        if (faults.injected()) {
          ASSERT_FALSE(nat.has_value());
          EXPECT_EQ(Nat::CreateError::kOutOfMemory, nat.error());
        } else {
          ASSERT_TRUE(nat.has_value());
        }
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
      allocations = faults.allocations();
      frees = faults.frees();
    }
    EXPECT_FALSE(threw) << "Create threw instead of returning kOutOfMemory";
    if (injected) {
      EXPECT_EQ(allocations, frees + 1) << "the refused allocation is the only one not freed";
    } else {
      EXPECT_EQ(allocations, frees) << "a destroyed engine left memory behind";
    }
  });
  EXPECT_GE(points, 7u);
}

// Binding creation allocates nothing: once Create has returned, outbound
// packets that create bindings, refusals on a full table, inbound packets with
// and without a binding, and expiry only use the memory Create committed --
// so a per-binding allocation failure cannot exist.
TEST(NatFaultTest, CreatingBindingsAllocatesNothing) {
  constexpr size_t kCapacity = 1024;
  Harness h({Pub(kPublic, {{1024, 65535, false}})}, /*timeout=*/100, kCapacity);
  constexpr size_t kFrames = 3000;
  std::vector<std::vector<uint8_t>> out, in;
  for (size_t i = 0; i < kFrames; i++) {
    const uint8_t proto = i % 3 == 0 ? 6 : i % 3 == 1 ? 17 : 1;
    out.push_back(Frame(proto, kInside + static_cast<uint32_t>(i / 1000),
                        static_cast<uint16_t>(2000 + i), kRemote, proto == 1 ? 8 : 443));
    in.push_back(Frame(17, kRemote, 53, kPublic, static_cast<uint16_t>(1024 + i)));
  }
  size_t translated = 0, full = 0, expired = 0;
  const fault_injection::AllocationFaults window;
  for (size_t i = 0; i < kFrames; i++) {
    // The first half at time 0 overfills the table; the second half follows
    // an expiry that emptied it, and overfills it again.
    const uint64_t now = i < kFrames / 2 ? 0 : 1000;
    if (i == kFrames / 2) {
      expired = h.nat->Expire(now, ~size_t{0});
    }
    const Verdict v = h.Send(out[i], Direction::kForward, now);
    translated += v == Verdict::kTranslated;
    full += v == Verdict::kFull;
    (void)h.Send(in[i], Direction::kReverse, now);
  }
  EXPECT_EQ(0u, window.allocations()) << "a binding operation allocated";
  EXPECT_EQ(kCapacity, expired);
  EXPECT_EQ(2 * kCapacity, translated);
  EXPECT_EQ(kFrames - 2 * kCapacity, full);
}

// A VLAN-tagged frame: every offset shifts by the tag, and the rewrite lands
// on the shifted headers with valid checksums.
TEST(NatTest, VlanTaggedFramesAreTranslated) {
  Harness h({Pub()});
  auto plain = Frame(6, kInside, 40000, kRemote, 443);
  std::vector<uint8_t> tagged(plain.begin(), plain.begin() + 12);
  tagged.insert(tagged.end(), {0x81, 0x00, 0x00, 0x05});
  tagged.insert(tagged.end(), plain.begin() + 12, plain.end());
  ASSERT_EQ(Verdict::kTranslated, h.Send(tagged, Direction::kForward, 0));
  std::vector<uint8_t> untagged(tagged.begin(), tagged.begin() + 12);
  untagged.insert(untagged.end(), tagged.begin() + 16, tagged.end());
  EXPECT_TRUE(ChecksumsValid(untagged));
  EXPECT_EQ(be32_t(kPublic), Src(untagged).addr);
  EXPECT_EQ(0x05, tagged[15]) << "the tag is untouched";
}

// A chained packet: the headers are in the first segment and the datagram
// continues past it. The lengths are checked against the whole packet and the
// rewrite (headers only) keeps the full checksum valid.
TEST(NatTest, ChainedPacketsAreTranslatedFromTheFirstSegment) {
  Harness h({Pub()});
  auto f = Frame(17, kInside, 40000, kRemote, 53);
  // Grow the payload to 200 bytes, then fix the lengths and checksums.
  f.resize(14 + 20 + 8 + 200, 0x5a);
  Put16(f, 16, static_cast<uint16_t>(20 + 8 + 200));
  Put16(f, 38, 8 + 200);
  auto *ip = reinterpret_cast<utils::Ipv4 *>(f.data() + 14);
  ip->checksum = 0;
  ip->checksum = utils::CalculateIpv4NoOptChecksum(*ip);
  auto *udp = reinterpret_cast<utils::Udp *>(f.data() + 34);
  udp->checksum = 0;
  udp->checksum = utils::CalculateIpv4UdpChecksum(*ip, *udp);
  const std::span<uint8_t> head(f.data(), 60);  // the first segment
  ParsedFlowPacket p;
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(head, p)) << "alone, the lengths lie";
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(head, p, f.size()));
  ASSERT_EQ(Verdict::kTranslated, h.nat->Translate(head, p, Direction::kForward, 0));
  EXPECT_TRUE(ChecksumsValid(f));
  // Headers that do not fit the first segment are still refused.
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(std::span<uint8_t>(f.data(), 38), p, f.size()));
}

TEST(NatTest, FullTablesDuplicateAddressesAndConflicts) {
  // A full table refuses new mappings with kFull.
  Harness small({Pub()}, 300 * kSec, /*cap=*/2);
  for (uint16_t i = 0; i < 2; i++) {
    auto f = Frame(17, kInside, static_cast<uint16_t>(5000 + i), kRemote, 53);
    ASSERT_EQ(Verdict::kTranslated, small.Send(f, Direction::kForward, 0));
  }
  auto f = Frame(17, kInside, 5002, kRemote, 53);
  EXPECT_EQ(Verdict::kFull, small.Send(f, Direction::kForward, 0));
  // One IP listed twice with overlapping ranges: its ports are handed out once.
  Harness dup({Pub(kPublic, {{2000, 2002, false}}), Pub(kPublic, {{2000, 2002, false}})});
  std::set<uint16_t> ports;
  for (uint16_t i = 0; i < 2; i++) {
    auto g = Frame(17, 0x0a000000u + i * 7, 5000, kRemote, 53);
    ASSERT_EQ(Verdict::kTranslated, dup.Send(g, Direction::kForward, 0));
    ports.insert(Src(g).port.value());
  }
  EXPECT_EQ(2u, ports.size());
  auto g = Frame(17, 0x0a000063, 5000, kRemote, 53);
  EXPECT_EQ(Verdict::kExhausted, dup.Send(g, Direction::kForward, 0));
  // A mapping whose external endpoint would be its own internal endpoint (the
  // pool is the inside address) cannot be stored under one key space:
  // kConflict, and the port is free again.
  Harness self({Pub(kInside, {{5000, 5001, false}})});
  auto inside = Frame(17, kInside, 5000, kRemote, 53);
  EXPECT_EQ(Verdict::kConflict, self.Send(inside, Direction::kForward, 0));
  auto other = Frame(17, kInside, 6000, kRemote, 53);
  ASSERT_EQ(Verdict::kTranslated, self.Send(other, Direction::kForward, 0));
  EXPECT_EQ(5000, Src(other).port.value());
  inside = Frame(17, kInside, 5000, kRemote, 53);
  EXPECT_EQ(Verdict::kConflict, self.Send(inside, Direction::kForward, 0))
      << "now another mapping's external endpoint";
  EXPECT_EQ(Nat::CapacityFor({Pub(), Pub(), Pub(0x01020304)}), 2u * 3 * 65536);
}

// Growth (TP5): while a larger table is adopted but nothing has moved yet,
// new bindings may only fill it up to its capacity counting the ones still in
// the old table, so every binding fits when migration runs; nothing is lost.
TEST(NatTest, GrowthNeverLosesABindingWhenCreatesOutrunMigration) {
  GrowableNat::Config c;
  c.addresses = {Pub()};
  c.capacity = 4;
  c.max_capacity = 16;  // the wheel holds 16: only the size check stops at 8
  c.granularity_shift = 0;
  c.seed = 7;
  auto nat = GrowableNat::Create(c).value();
  auto send = [&](uint16_t port) {
    auto f = Frame(17, kInside, port, kRemote, 53);
    ParsedFlowPacket p;
    EXPECT_EQ(ParseStatus::kOk, ParseFrame(f, p));
    return nat->Translate(f, p, Direction::kForward, 0);
  };
  for (uint16_t port = 1000; port < 1003; port++) ASSERT_EQ(Verdict::kTranslated, send(port));
  ASSERT_TRUE(nat->NeedsGrowth()) << "3 of 4";
  ASSERT_EQ(8u, nat->GrowthTarget());
  nat->Adopt(GrowableNat::NewTable(nat->GrowthTarget()));
  ASSERT_TRUE(nat->migrating());
  // No migration step: creates go to the new table until both together fill it.
  uint16_t port = 1003;
  while (send(port) == Verdict::kTranslated) port++;
  EXPECT_EQ(8u, nat->size()) << "3 old + 5 new: the new table's capacity, no more";
  EXPECT_EQ(Verdict::kFull, send(port));
  std::unique_ptr<GrowableNat::Table> old;
  while ((old = nat->MigrateSome(1)) == nullptr) {
  }
  EXPECT_EQ(0u, old->size()) << "every binding moved";
  EXPECT_FALSE(nat->migrating());
  EXPECT_EQ(8u, nat->size());
  for (uint16_t q = 1000; q < port; q++) {
    EXPECT_TRUE(nat->Find(Endpoint{be32_t(kInside), be16_t(q), 17}) != nullptr) << q;
  }
  EXPECT_TRUE(nat->NeedsGrowth()) << "8 of 8: the next growth (to 16) is due";
}

// A batch gives what packet-by-packet translation gives, including two
// packets of one new flow in the same batch (one mapping, not a refusal).
TEST(NatTest, BatchMatchesPacketByPacket) {
  Harness one({Pub()}), many({Pub()});  // same seed: same port choices
  std::mt19937 rng(21);
  for (int round = 0; round < 50; round++) {
    std::vector<std::vector<uint8_t>> a, b;
    for (int i = 0; i < 32; i++) {
      const uint32_t src = 0x0a000000u + rng() % 8;
      const uint16_t sport = static_cast<uint16_t>(rng() % 3 == 0 ? 0 : 30000 + rng() % 6);
      auto f = rng() % 5 == 0 ? Frame(17, kRemote, 53, kPublic, static_cast<uint16_t>(1024 + rng() % 100))
                              : Frame(17, src, sport, kRemote, 53);
      a.push_back(f);
      b.push_back(f);
    }
    for (const Direction dir : {Direction::kForward, Direction::kReverse}) {
      std::vector<Verdict> want;
      for (auto &f : a) {
        want.push_back(one.Send(f, dir, round));
      }
      std::span<uint8_t> frames[32];
      ParsedFlowPacket ps[32];
      bool good[32];
      Verdict got[32];
      for (int i = 0; i < 32; i++) {
        frames[i] = b[i];
        good[i] = ParseFrame(frames[i], ps[i]) == ParseStatus::kOk;
      }
      many.nat->TranslateBatch(frames, ps, good, dir, round, got);
      for (int i = 0; i < 32; i++) {
        ASSERT_EQ(want[i], got[i]) << "round " << round << " packet " << i;
        ASSERT_EQ(a[i], b[i]) << "same rewrite";
      }
    }
  }
  EXPECT_EQ(one.nat->size(), many.nat->size());
}

// The legacy module's algorithm (modules/nat.cc before M18), as the
// compatibility oracle: an unordered map with forward and reverse entries,
// the address by hash, a random start port and linear probing.
class LegacyNat {
 public:
  LegacyNat(std::vector<be32_t> addrs, uint64_t seed) : addrs_(std::move(addrs)), rng_(seed) {}
  std::optional<Endpoint> Forward(const Endpoint &in) {
    auto it = map_.find(Key(in));
    if (it != map_.end()) return it->second;
    if ((in.protocol == 6 || in.protocol == 17) && in.port == be16_t(0)) return std::nullopt;
    const size_t a = rte_hash_crc(&in.addr, sizeof(be32_t), 0) % addrs_.size();
    Endpoint ext{addrs_[a], be16_t(0), in.protocol};
    uint16_t min, range;
    if (in.protocol == 1) {
      min = 0;
      range = 65535;
    } else if (in.port.value() >= 1024) {
      min = 1024;
      range = 65535 - 1024 + 1;
    } else {
      min = 0;
      range = 1023;
    }
    uint16_t port = static_cast<uint16_t>(min + rng_.GetRange(range));
    for (int t = 0; t < 128; t++) {
      ext.port = be16_t(port);
      if (!map_.count(Key(ext))) {
        map_[Key(ext)] = in;
        map_[Key(in)] = ext;
        return ext;
      }
      if (++port == 0 || port >= min + range) port = min;
    }
    return std::nullopt;
  }
  std::optional<Endpoint> Reverse(const Endpoint &ext) {
    auto it = map_.find(Key(ext));
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }

 private:
  static uint64_t Key(const Endpoint &e) {
    uint64_t k;
    std::memcpy(&k, &e, 8);
    return k;
  }
  std::vector<be32_t> addrs_;
  Random rng_;
  std::unordered_map<uint64_t, Endpoint> map_;
};

// Both implementations, fed the same outbound stream (within the timeout), must
// agree on what is translated, which external address an internal address uses,
// the port class, and that every external endpoint leads back to its internal
// one. Ports differ (each picks at random).
TEST(NatTest, DifferentialAgainstTheLegacyAlgorithm) {
  const std::vector<be32_t> addrs = {be32_t(0xc6336401), be32_t(0xc6336402), be32_t(0xc6336403)};
  std::vector<ExternalAddress> ext;
  for (const auto a : addrs) ext.push_back({a, {{0, 65535, false}}});
  Harness h(ext, 300 * kSec, /*cap=*/65536);  // legacy's table is unbounded
  LegacyNat legacy(addrs, 99);
  std::mt19937 rng(0xd1ff);
  int translated = 0;
  for (int i = 0; i < 5000; i++) {
    const uint8_t proto = std::array<uint8_t, 3>{6, 17, 1}[rng() % 3];
    const uint32_t src = 0x0a000000u + rng() % 64;
    const uint16_t sport = proto == 1 ? static_cast<uint16_t>(rng() % 50)
                                      : static_cast<uint16_t>(rng() % 8 == 0 ? rng() % 1024 : rng() % 200 + 30000);
    const uint8_t icmp_type = std::array<uint8_t, 5>{0, 8, 13, 15, 16}[rng() % 5];
    auto f = Frame(proto, src, sport, kRemote, proto == 1 ? icmp_type : 443);
    const Endpoint in = Src(f);
    const Verdict v = h.Send(f, Direction::kForward, static_cast<uint64_t>(i) * 1000);
    const auto old = legacy.Forward(in);
    ASSERT_EQ(old.has_value(), v == Verdict::kTranslated)
        << i << " verdict " << int(v) << " proto " << int(proto) << " port " << sport << " type " << int(icmp_type);
    if (!old) continue;
    translated++;
    const Endpoint mine = Src(f);
    ASSERT_EQ(old->addr, mine.addr) << "the same external address for an internal address";
    if (proto != 1) {
      ASSERT_EQ(in.port.value() < 1024, mine.port.value() < 1024) << "port class";
      ASSERT_EQ(in.port.value() < 1024, old->port.value() < 1024);
    }
    ASSERT_EQ(in, legacy.Reverse(*old).value());
    ASSERT_EQ(in, h.nat->Find(mine)->internal);
  }
  EXPECT_GT(translated, 4000);
}

}  // namespace
}  // namespace bess::nat
