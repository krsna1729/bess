// SPDX-License-Identifier: BSD-3-Clause

// The sample plugin's SequentialUpdate in process, linked into the test
// rather than loaded (the plugin in a daemon is tests/live/
// sample_plugin_test.cc): exactly `size` bytes are written, a packet whose
// first segment is shorter than a field is dropped, a shared payload is
// copied first, and Init refuses an offset outside the data room.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "packet.h"
#include "pb/supdate_msg.pb.h"
#include "testing/module_harness.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;
using sample::supdate::pb::SequentialUpdateArg;

struct F {
  int64_t offset;
  uint64_t size;
  uint64_t min;
  uint64_t max;
};

SequentialUpdateArg Fields(std::initializer_list<F> fields) {
  SequentialUpdateArg arg;
  for (const F &f : fields) {
    auto *field = arg.add_fields();
    field->set_offset(f.offset);
    field->set_size(f.size);
    field->set_min(f.min);
    field->set_max(f.max);
  }
  return arg;
}

// 0, 1, 2, ... as an `n`-byte frame.
Bytes Counting(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  return b;
}

// `frame` through `m`: what left on gate 0 (empty: dropped).
Bytes ThroughPacket(ModuleHarness &h, Module *m, bess::PacketHandle frame) {
  auto out = h.RunPackets(m, 0, {frame}, m, {0});
  return out[0].empty() ? Bytes() : out[0][0];
}

// The sequence counts up from min and wraps after max; each value is written
// big-endian into exactly `size` bytes.
TEST(SequentialUpdateTest, WritesTheSequenceExactly) {
  ModuleHarness h;
  Module *m = h.Create("SequentialUpdate",
                       Fields({{.offset = 10, .size = 2, .min = 0x0102, .max = 0x0103},
                               {.offset = 20, .size = 1, .min = 9, .max = 9}}));
  ASSERT_NE(nullptr, m);
  for (uint8_t low : {0x02, 0x03, 0x02}) {
    Bytes expected = Counting(30);
    expected[10] = 0x01;
    expected[11] = low;
    expected[20] = 9;
    EXPECT_EQ(expected, ThroughPacket(h, m, MakePacket(Counting(30))));
  }
}

// A 1-byte field at 59 fits a 60-byte frame and a 2-byte field at 58 too;
// one byte shorter, the frame is dropped rather than written past its data
// (the old code wrote 4 bytes at the offset whatever the size, and wrote past
// the data of both).
TEST(SequentialUpdateTest, AShortFrameIsDropped) {
  ModuleHarness h;
  for (uint64_t size : {1u, 2u}) {
    const int64_t offset = static_cast<int64_t>(60 - size);
    Module *m = h.Create("SequentialUpdate",
                         Fields({{.offset = offset, .size = size, .min = 0x55, .max = 0x55}}));
    ASSERT_NE(nullptr, m);
    Bytes expected = Counting(60);
    if (size == 2) {
      expected[58] = 0;
    }
    expected[59] = 0x55;
    EXPECT_EQ(expected, ThroughPacket(h, m, MakePacket(Counting(60)))) << "size " << size;
    EXPECT_TRUE(ThroughPacket(h, m, MakePacket(Counting(59))).empty()) << "size " << size;
  }
}

// Only the first segment counts: a chain whose first segment ends inside the
// field is dropped; one that holds it is written.
TEST(SequentialUpdateTest, AChainSplitInsideAFieldIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("SequentialUpdate",
                       Fields({{.offset = 30, .size = 4, .min = 0x01020304, .max = 0x01020304}}));
  ASSERT_NE(nullptr, m);
  Bytes expected = Counting(60);
  expected[30] = 1;
  expected[31] = 2;
  expected[32] = 3;
  expected[33] = 4;
  for (size_t split : {30u, 33u}) {
    EXPECT_TRUE(ThroughPacket(h, m, MakePacket(Counting(60), split)).empty())
        << "first segment of " << split << " bytes";
  }
  for (size_t split : {34u, 50u}) {
    EXPECT_EQ(expected, ThroughPacket(h, m, MakePacket(Counting(60), split)))
        << "first segment of " << split << " bytes";
  }
}

// A clone's payload is copied before the write: the other holder keeps its
// bytes. The old code wrote the shared bytes.
TEST(SequentialUpdateTest, ASharedFrameIsCopiedBeforeTheWrite) {
  ModuleHarness h;
  Module *m = h.Create("SequentialUpdate", Fields({{.offset = 30, .size = 2, .min = 7, .max = 7}}));
  ASSERT_NE(nullptr, m);
  const Bytes frame = Counting(60);
  Bytes expected = frame;
  expected[30] = 0;
  expected[31] = 7;
  for (size_t split : {0u, 50u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ(expected, ThroughPacket(h, m, clone)) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

// A negative offset (the old check, `offset + size > 2048` in size_t, let -1
// through as 0) and an offset past the data room are refused.
TEST(SequentialUpdateTest, InitRefusesAnOffsetOutsideTheDataRoom) {
  ModuleHarness h;
  EXPECT_NE("", h.CreateError("SequentialUpdate",
                              Fields({{.offset = -1, .size = 1, .min = 1, .max = 1}})));
  EXPECT_NE("", h.CreateError("SequentialUpdate",
                              Fields({{.offset = 2047, .size = 2, .min = 1, .max = 1}})));
  EXPECT_EQ("", h.CreateError("SequentialUpdate",
                              Fields({{.offset = 2046, .size = 2, .min = 1, .max = 1}})));
}

}  // namespace
