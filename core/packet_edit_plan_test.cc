// SPDX-License-Identifier: BSD-3-Clause

// EditPlan (M13, D-063): every case's plan produces the same bytes as the
// hand-written C++ and leaves valid checksums; build-time merging; refusals
// that change nothing.

#include "packet_edit_plan.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "packet_edit_plan_cases.h"
#include "packet_pool.h"

namespace bess::packet {
namespace {

namespace ec = edit_cases;

class EditPlanTest : public ::testing::Test {
 protected:
  PlainPacketPool pool_{16, -1, 512};

  PacketHandle Load(const std::vector<uint8_t> &bytes) {
    PacketHandle m = pool_.Alloc(bytes.size());
    EXPECT_NE(nullptr, m);
    std::memcpy(rte_pktmbuf_mtod(m, uint8_t *), bytes.data(), bytes.size());
    return m;
  }
  static std::vector<uint8_t> Bytes(PacketHandle m) {
    const auto *p = rte_pktmbuf_mtod(m, const uint8_t *);
    return std::vector<uint8_t>(p, p + m->data_len);
  }
};

TEST_F(EditPlanTest, NatPlanMatchesHandWrittenAndKeepsChecksumsValid) {
  const auto nat = ec::NatParams();
  PacketHandle a = Load(ec::TcpPacket()), b = Load(ec::TcpPacket());
  ec::NatHand(rte_pktmbuf_mtod(a, uint8_t *), nat);
  const EditPlan plan = ec::NatPlan(nat);
  ASSERT_TRUE(plan.Apply(b).has_value());
  EXPECT_EQ(Bytes(a), Bytes(b));
  const auto *ip = rte_pktmbuf_mtod_offset(b, const utils::Ipv4 *, ec::kIp);
  const auto *tcp = rte_pktmbuf_mtod_offset(b, const utils::Tcp *, ec::kL4);
  EXPECT_TRUE(utils::VerifyIpv4NoOptChecksum(*ip));
  EXPECT_TRUE(utils::VerifyIpv4TcpChecksum(*ip, *tcp));
  // Writes stay two (not adjacent); three checksum adjustments fold into two.
  EXPECT_EQ(4u, plan.steps());
  // The trusted path (no per-packet checks) writes the same bytes.
  PacketHandle c = Load(ec::TcpPacket());
  plan.ApplyTrusted(PacketRef(c));
  EXPECT_EQ(Bytes(b), Bytes(c));
  PacketFree(a);
  PacketFree(b);
  PacketFree(c);
}

TEST_F(EditPlanTest, VxlanEncapAndDecapMatchHandWritten) {
  const auto h = ec::VxlanHeader();
  PacketHandle a = Load(ec::TcpPacket()), b = Load(ec::TcpPacket());
  ec::VxlanEncapHand(a, h);
  ASSERT_TRUE(ec::VxlanEncapPlan(h).Apply(b).has_value());
  EXPECT_EQ(a->pkt_len, b->pkt_len);
  EXPECT_EQ(Bytes(a), Bytes(b));
  PacketHandle c = Load(ec::TcpPacket());
  ec::VxlanEncapPlan(h).ApplyTrusted(PacketRef(c));
  EXPECT_EQ(Bytes(a), Bytes(c));
  PacketFree(c);
  EXPECT_TRUE(utils::VerifyIpv4NoOptChecksum(
      *rte_pktmbuf_mtod_offset(b, const utils::Ipv4 *, 14)));

  ec::VxlanDecapHand(a);
  ASSERT_TRUE(ec::VxlanDecapPlan().Apply(b).has_value());
  EXPECT_EQ(Bytes(a), Bytes(b));
  EXPECT_EQ(ec::TcpPacket(), Bytes(b));
  PacketFree(a);
  PacketFree(b);
}

TEST_F(EditPlanTest, VfpRewriteMatchesHandWrittenAndMergesItsChecksumSteps) {
  const auto v = ec::VfpParams();
  PacketHandle a = Load(ec::TcpPacket()), b = Load(ec::TcpPacket());
  ec::VfpHand(rte_pktmbuf_mtod(a, uint8_t *), v);
  const EditPlan plan = ec::VfpPlan(v);
  ASSERT_TRUE(plan.Apply(b).has_value());
  EXPECT_EQ(Bytes(a), Bytes(b));
  const auto *ip = rte_pktmbuf_mtod_offset(b, const utils::Ipv4 *, ec::kIp);
  EXPECT_TRUE(utils::VerifyIpv4NoOptChecksum(*ip));
  EXPECT_TRUE(utils::VerifyIpv4TcpChecksum(
      *ip, *rte_pktmbuf_mtod_offset(b, const utils::Tcp *, ec::kL4)));
  // MACs, address, port: three writes; the IP and TCP checksums: two steps.
  EXPECT_EQ(5u, plan.steps());
  PacketFree(a);
  PacketFree(b);
}

TEST_F(EditPlanTest, UpfOuterReplacementIsOneInPlacePrefixChange) {
  const auto eth = ec::InnerEthernet();
  std::vector<uint8_t> in(ec::kGtpOuter, 0xee);
  const auto inner = ec::TcpPacket();
  in.insert(in.end(), inner.begin() + 14, inner.end());  // an inner IPv4 packet
  PacketHandle a = Load(in), b = Load(in);
  ec::UpfHand(a, eth);
  const EditPlan plan = ec::UpfPlan(eth);
  EXPECT_EQ(ec::kGtpOuter, plan.remove_bytes());
  EXPECT_EQ(14, plan.prepend_bytes());
  ASSERT_TRUE(plan.Apply(b).has_value());
  EXPECT_EQ(Bytes(a), Bytes(b));
  EXPECT_EQ(a->data_off, b->data_off);
  PacketFree(a);
  PacketFree(b);
}

TEST_F(EditPlanTest, AdjacentWritesMergeIntoOneStep) {
  const uint8_t x[2] = {1, 2}, y[3] = {3, 4, 5}, z[1] = {6};
  const EditPlan plan =
      EditPlanBuilder().Write(10, x).Write(12, y).Write(11, z).Build().value();
  ASSERT_EQ(1u, plan.steps());
  PacketHandle m = Load(std::vector<uint8_t>(32, 0));
  ASSERT_TRUE(plan.Apply(m).has_value());
  const auto b = Bytes(m);
  EXPECT_EQ((std::vector<uint8_t>{1, 6, 3, 4, 5}),
            std::vector<uint8_t>(b.begin() + 10, b.begin() + 15));
  PacketFree(m);
}

TEST_F(EditPlanTest, RefusalsChangeNothing) {
  // Shared storage: the caller must make it writable first.
  PacketHandle m = Load(ec::TcpPacket());
  PacketHandle clone = rte_pktmbuf_clone(m, m->pool);
  ASSERT_NE(nullptr, clone);
  const auto before = Bytes(m);
  const auto r = ec::NatPlan(ec::NatParams()).Apply(m);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(EditError::kNeedsReshape, r.error());
  EXPECT_EQ(before, Bytes(m));
  PacketFree(clone);

  // Shorter than the plan's range or its removal.
  PacketHandle tiny = Load(std::vector<uint8_t>(20, 0));
  EXPECT_EQ(EditError::kNeedsReshape, ec::NatPlan(ec::NatParams()).Apply(tiny).error());
  EXPECT_EQ(EditError::kTooShort, ec::VxlanDecapPlan().Apply(tiny).error());
  EXPECT_EQ(20u, tiny->pkt_len);

  // More headroom than the packet has (128 bytes in this pool), within the
  // plan's data limit.
  std::vector<uint8_t> big(150, 0);
  const auto plan = EditPlanBuilder().Prepend(big).Build().value();
  const uint16_t off = m->data_off;
  EXPECT_EQ(EditError::kNoHeadroom, plan.Apply(m).error());
  EXPECT_EQ(off, m->data_off);
  PacketFree(m);
  PacketFree(tiny);
}

TEST(EditPlanBuilderTest, RejectsWhatItCannotRepresent) {
  EditPlanBuilder too_many;
  for (uint16_t i = 0; i < 20; i++) {
    const uint8_t b = 1;
    too_many.Write(static_cast<uint16_t>(i * 4), std::span<const uint8_t>(&b, 1));
  }
  EXPECT_EQ(EditBuildError::kTooManySteps, too_many.Build().error());

  std::vector<uint8_t> lots(EditPlan::kMaxData + 1, 1);
  EXPECT_EQ(EditBuildError::kTooMuchData, EditPlanBuilder().Write(0, lots).Build().error());

  // An IPv4 checksum over a header the plan does not fix is refused: its sum
  // could not be precomputed.
  EXPECT_EQ(EditBuildError::kOutOfRange,
            EditPlanBuilder().Ipv4HeaderChecksum(14).Build().error());

  // Nor over a header a Copy writes into (inheriting the inner TOS into an
  // outer header): the copied byte is not known when the sum is computed.
  const auto h = edit_cases::VxlanHeader();
  EXPECT_TRUE(EditPlanBuilder().Prepend(h).Ipv4HeaderChecksum(14).Build().has_value());
  EXPECT_EQ(EditBuildError::kOutOfRange, EditPlanBuilder()
                                             .Prepend(h)
                                             .Copy(15, 51, 1)
                                             .Ipv4HeaderChecksum(14)
                                             .Build()
                                             .error());
}

}  // namespace
}  // namespace bess::packet
