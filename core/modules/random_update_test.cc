// SPDX-License-Identifier: BSD-3-Clause

// RandomUpdate in process: each field overwrites `size` bytes at `offset`
// with a random value in [min, max] (min == max here, so the packets are
// known). D-129: "<name>/fields" holds the whole field list (setting it
// replaces the list); `add` and `clear` went with the legacy service (D-140).

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "packet.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

struct F {
  int64_t offset;
  uint64_t size;
  uint64_t min;
  uint64_t max;
};

bess::pb::RandomUpdateArg Fields(std::initializer_list<F> fields) {
  bess::pb::RandomUpdateArg arg;
  for (const F &f : fields) {
    auto *field = arg.add_fields();
    field->set_offset(f.offset);
    field->set_size(f.size);
    field->set_min(f.min);
    field->set_max(f.max);
  }
  return arg;
}

// Whether the codec bound to `resource` requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  EXPECT_NE(nullptr, r) << resource;
  if (r == nullptr) {
    return false;
  }
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << resource;
  return codec != nullptr && codec->requires_paused_workers();
}

// `in` after one pass through `m` (gate 0).
Bytes Through(ModuleHarness &h, Module *m, const Bytes &in) {
  auto out = h.Run(m, 0, {in}, {0});
  EXPECT_EQ(1u, out[0].size());
  return out[0].empty() ? Bytes() : out[0][0];
}

TEST(RandomUpdateTest, FieldsResourceReplacesTheList) {
  const Bytes zeros(60, 0);
  ModuleHarness h;
  const auto initial = Fields({{.offset = 30, .size = 1, .min = 0x11, .max = 0x11}});
  Module *ru = h.Create("RandomUpdate", initial, "rupd_res");
  ASSERT_NE(nullptr, ru);
  const std::string fields = "rupd_res/fields";
  EXPECT_TRUE(RequiresPausedWorkers(fields));
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &initial}})) << "Init's fields";
  Bytes expected = zeros;
  expected[30] = 0x11;
  EXPECT_EQ(expected, Through(h, ru, zeros));

  const auto two = Fields({{.offset = 0, .size = 1, .min = 0x42, .max = 0x42},
                           {.offset = 20, .size = 2, .min = 0x1234, .max = 0x1234}});
  EXPECT_EQ("", h.SetResource(fields, two));
  expected = zeros;
  expected[0] = 0x42;
  expected[20] = 0x12;
  expected[21] = 0x34;
  EXPECT_EQ(expected, Through(h, ru, zeros)) << "the Init field was replaced";
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &two}}));

  EXPECT_NE("", h.SetResource(fields, Fields({{.offset = 0, .size = 1, .min = 1, .max = 1},
                                              {.offset = 0, .size = 3, .min = 1, .max = 1}})));
  EXPECT_NE("", h.SetResource(fields, Fields({{.offset = 0, .size = 1, .min = 9, .max = 8}})));
  EXPECT_NE("", h.SetResource(fields, Fields({{.offset = 4096, .size = 1, .min = 1, .max = 1}})));
  bess::pb::RandomUpdateArg too_many;
  for (int i = 0; i < 17; i++) {
    too_many.MergeFrom(Fields({{.offset = i, .size = 1, .min = 1, .max = 1}}));
  }
  EXPECT_NE("", h.SetResource(fields, too_many));
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &two}})) << "refused sets change nothing";
  EXPECT_EQ(expected, Through(h, ru, zeros));

  const bess::pb::RandomUpdateArg none;
  EXPECT_EQ("", h.SetResource(fields, none));
  EXPECT_EQ(zeros, Through(h, ru, zeros));
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &none}}));
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

// Exactly `size` bytes are written: a 1-byte field at 59 fits a 60-byte
// frame and a 2-byte field at 58 too; one byte shorter, the frame is dropped
// rather than written past its data (the old code wrote 4 bytes at the
// offset whatever the size, and wrote past the data of both).
TEST(RandomUpdateTest, AFieldIsWrittenExactlyAndAShortFrameIsDropped) {
  ModuleHarness h;
  for (uint64_t size : {1u, 2u}) {
    const int64_t offset = static_cast<int64_t>(60 - size);
    const uint64_t value = size == 1 ? 0xcd : 0xabcd;
    Module *m = h.Create("RandomUpdate", Fields({{.offset = offset, .size = size, .min = value,
                                                  .max = value}}));
    ASSERT_NE(nullptr, m);
    Bytes expected = Counting(60);
    if (size == 1) {
      expected[59] = 0xcd;
    } else {
      expected[58] = 0xab;
      expected[59] = 0xcd;
    }
    EXPECT_EQ(expected, ThroughPacket(h, m, MakePacket(Counting(60)))) << "size " << size;
    EXPECT_TRUE(ThroughPacket(h, m, MakePacket(Counting(59))).empty()) << "size " << size;
  }
}

// Only the first segment counts (D-121): a chain whose first segment ends
// inside the field is dropped; one that holds it is written.
TEST(RandomUpdateTest, AChainSplitInsideAFieldIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("RandomUpdate",
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

// Every field's bytes are checked before the first is written: a frame that
// holds the first field but not the second is dropped, wherever it sits in
// the batch, and the written frames leave in order with both fields set.
TEST(RandomUpdateTest, AFrameShortOfALaterFieldIsDroppedAnywhereInTheBatch) {
  ModuleHarness h;
  Module *m = h.Create("RandomUpdate", Fields({{.offset = 0, .size = 1, .min = 0x42, .max = 0x42},
                                               {.offset = 40, .size = 2, .min = 0x1234,
                                                .max = 0x1234}}));
  ASSERT_NE(nullptr, m);
  const std::vector<std::vector<size_t>> patterns = {{}, {0}, {2}, {4}, {0, 4}};
  for (const std::vector<size_t> &shorts : patterns) {
    SCOPED_TRACE(::testing::PrintToString(shorts));
    std::vector<Bytes> in;
    std::vector<Bytes> want;
    for (size_t i = 0; i < 5; i++) {
      const bool short_frame = std::find(shorts.begin(), shorts.end(), i) != shorts.end();
      // 41 bytes hold the first field, not the second (it ends at 42).
      Bytes frame(short_frame ? 41 : 60, static_cast<uint8_t>(i + 1));
      in.push_back(frame);
      if (!short_frame) {
        frame[0] = 0x42;
        frame[40] = 0x12;
        frame[41] = 0x34;
        want.push_back(frame);
      }
    }
    auto out = h.Run(m, 0, in, {0});
    EXPECT_EQ(want, out[0]);
  }
}

// A clone's payload is copied before the write: the other holder keeps its
// bytes. The old code wrote the shared bytes.
TEST(RandomUpdateTest, ASharedFrameIsCopiedBeforeTheWrite) {
  ModuleHarness h;
  Module *m = h.Create("RandomUpdate", Fields({{.offset = 30, .size = 2, .min = 7, .max = 7}}));
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

// Init refuses what the resource refuses: a negative offset (the old check,
// `offset + size > 2048` in size_t, let -1 through as 0), an offset past the
// data room, and more than 16 fields (the old Init wrote past its array).
TEST(RandomUpdateTest, InitRefusesAnOffsetOutsideTheDataRoomAndTooManyFields) {
  ModuleHarness h;
  EXPECT_NE("", h.CreateError("RandomUpdate", Fields({{.offset = -1, .size = 1, .min = 1, .max = 1}})));
  EXPECT_NE("", h.CreateError("RandomUpdate",
                              Fields({{.offset = 2047, .size = 2, .min = 1, .max = 1}})));
  EXPECT_EQ("", h.CreateError("RandomUpdate",
                              Fields({{.offset = 2046, .size = 2, .min = 1, .max = 1}})));
  bess::pb::RandomUpdateArg too_many;
  for (int i = 0; i < 17; i++) {
    too_many.MergeFrom(Fields({{.offset = i, .size = 1, .min = 1, .max = 1}}));
  }
  EXPECT_NE("", h.CreateError("RandomUpdate", too_many));
}

}  // namespace
