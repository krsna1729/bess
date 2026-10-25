// SPDX-License-Identifier: BSD-3-Clause

// Measure's commands as control_v2 runs them (D-129): both are actions
// (they read and clear counters, not configuration); get_summary answers a
// MeasureCommandGetSummaryResponse. The in-packet stamp at `offset` is read
// only from the first segment, and Init bounds the offset by the data room.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "module.h"
#include "modules/timestamp.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::MakeShortPacket;
using bess::testing::ModuleHarness;

bess::pb::MeasureArg OffsetArg(uint64_t offset) {
  bess::pb::MeasureArg arg;
  arg.set_offset(offset);
  return arg;
}

// `len` zero bytes with Timestamp's stamp at `at`, time 1 ns (in the past,
// so it is measured).
Bytes Stamped(size_t len, size_t at) {
  Bytes b(len, 0);
  const Timestamp::MarkerType marker = Timestamp::kMarker;
  const uint64_t time = 1;
  std::memcpy(b.data() + at, &marker, sizeof(marker));
  std::memcpy(b.data() + at + sizeof(marker), &time, sizeof(time));
  return b;
}

// The latency samples `m` holds; clears them.
uint64_t TakeLatencyCount(ModuleHarness &h, Module *m) {
  CommandResponse r = h.Action(m, "get_summary", bess::pb::MeasureCommandGetSummaryArg());
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  bess::pb::MeasureCommandGetSummaryResponse summary;
  EXPECT_TRUE(r.data().UnpackTo(&summary));
  EXPECT_EQ(0, h.Action(m, "clear", bess::pb::EmptyArg()).error().code());
  return summary.latency().count();
}

// `pkt` through `m`: whether it was measured. Measure forwards every packet.
bool Measured(ModuleHarness &h, Module *m, bess::PacketHandle pkt) {
  EXPECT_EQ(1u, h.RunPackets(m, 0, {pkt}, m, {0})[0].size()) << "Measure forwards every packet";
  return TakeLatencyCount(h, m) == 1;
}

// Packets pass through and are counted; with no timestamp attribute writer
// and no timestamp in the payload none is sampled for latency.
TEST(MeasureTest, GetSummaryAndClearAreActions) {
  ModuleHarness h;
  Module *measure = h.Create("Measure", bess::pb::MeasureArg());
  ASSERT_NE(nullptr, measure);

  const std::vector<Bytes> in(5, Bytes(64, 0));
  EXPECT_EQ(in.size(), h.Run(measure, 0, in, {0})[0].size());

  CommandResponse r =
      h.Action(measure, "get_summary", bess::pb::MeasureCommandGetSummaryArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  bess::pb::MeasureCommandGetSummaryResponse summary;
  ASSERT_TRUE(r.data().UnpackTo(&summary));
  EXPECT_EQ(5u, summary.packets());
  EXPECT_EQ(0u, summary.latency().count());

  r = h.Action(measure, "clear", bess::pb::EmptyArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  r = h.Action(measure, "get_summary", bess::pb::MeasureCommandGetSummaryArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  ASSERT_TRUE(r.data().UnpackTo(&summary));
  EXPECT_EQ(0u, summary.packets());

  // An invalid argument is still refused through the action surface.
  bess::pb::MeasureCommandGetSummaryArg bad;
  bad.add_latency_percentiles(101.0);
  EXPECT_EQ(EINVAL, h.Action(measure, "get_summary", bad).error().code());
}

// A packet that ends before the stamp's end (here 54: offset 42 + 12) has
// no stamp: at every length from 1 to 53 it is forwarded, not measured. The
// old code read the stamp past the data and measured each from 42 bytes on.
TEST(MeasureTest, AFrameShorterThanTheStampIsNotMeasured) {
  ModuleHarness h;
  Module *m = h.Create("Measure", OffsetArg(42));
  ASSERT_NE(nullptr, m);
  const Bytes full = Stamped(60, 42);
  for (size_t len = 1; len < 54; len++) {
    EXPECT_FALSE(Measured(h, m, MakeShortPacket(full, len))) << len << " bytes";
  }
  EXPECT_TRUE(Measured(h, m, MakeShortPacket(full, 54)));
  EXPECT_TRUE(Measured(h, m, MakePacket(full)));
}

// Only the first segment counts (D-121): a chain whose first segment ends
// inside the stamp is not measured; one that holds it is. The old code
// measured each.
TEST(MeasureTest, AChainShortInItsFirstSegmentIsNotMeasured) {
  ModuleHarness h;
  Module *m = h.Create("Measure", OffsetArg(42));
  ASSERT_NE(nullptr, m);
  const Bytes full = Stamped(60, 42);
  for (size_t split : {20u, 42u, 46u, 53u}) {
    EXPECT_FALSE(Measured(h, m, MakeShortPacket(full, split, true)))
        << "first segment of " << split << " bytes";
  }
  EXPECT_TRUE(Measured(h, m, MakeShortPacket(full, 54, true)));
}

// The stamp must fit the packet data room: the last offset is the room less
// 12, and a full-room packet stamped there is measured (one byte short, it
// is not). One more is refused, as is an offset past 16 bits that the old
// code cut to a uint16_t. The old code accepted both.
TEST(MeasureTest, TheOffsetAtItsLimit) {
  ModuleHarness h;
  const size_t room = bess::PacketPool::DefaultDataRoomSize();
  const size_t last = room - Timestamp::kStampSize;
  Module *m = h.Create("Measure", OffsetArg(last));
  ASSERT_NE(nullptr, m);
  const Bytes full = Stamped(room, last);
  EXPECT_TRUE(Measured(h, m, MakePacket(full)));
  EXPECT_FALSE(Measured(h, m, MakeShortPacket(full, room - 1)));
  EXPECT_EQ(EINVAL, h.CreateFailure("Measure", OffsetArg(last + 1)).code());
  EXPECT_EQ(EINVAL, h.CreateFailure("Measure", OffsetArg(65536 + 42)).code());
}

// Every packet leaves once, stamped, short or not: N in = N out.
TEST(MeasureTest, EveryPacketIsForwarded) {
  ModuleHarness h;
  Module *m = h.Create("Measure", OffsetArg(42));
  ASSERT_NE(nullptr, m);
  const std::vector<Bytes> in = {Stamped(60, 42), Bytes(10, 0), Bytes(53, 0), Stamped(54, 42)};
  EXPECT_EQ(in.size(), h.Run(m, 0, in, {0})[0].size());
  EXPECT_EQ(0u, m->deadends());
  EXPECT_EQ(2u, TakeLatencyCount(h, m));
}

}  // namespace
