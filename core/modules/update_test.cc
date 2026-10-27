// SPDX-License-Identifier: BSD-3-Clause

// Update in process: each field overwrites `size` bytes at `offset` with its
// value. D-129: "<name>/fields" holds the whole field list (setting it
// replaces the list); `add` and `clear` went with the legacy service (D-140).

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
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
  uint64_t value;
};

bess::pb::UpdateArg Fields(std::initializer_list<F> fields) {
  bess::pb::UpdateArg arg;
  for (const F &f : fields) {
    auto *field = arg.add_fields();
    field->set_offset(f.offset);
    field->set_size(f.size);
    field->set_value(f.value);
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

TEST(UpdateTest, FieldsResourceReplacesTheList) {
  const Bytes zeros(60, 0);
  ModuleHarness h;
  const auto initial = Fields({{.offset = 30, .size = 1, .value = 0x11}});
  Module *u = h.Create("Update", initial, "upd_res");
  ASSERT_NE(nullptr, u);
  const std::string fields = "upd_res/fields";
  EXPECT_TRUE(RequiresPausedWorkers(fields));
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &initial}})) << "Init's fields";
  Bytes expected = zeros;
  expected[30] = 0x11;
  EXPECT_EQ(expected, Through(h, u, zeros));

  const auto two = Fields({{.offset = 0, .size = 2, .value = 0xabcd},
                           {.offset = 10, .size = 1, .value = 0x7f}});
  EXPECT_EQ("", h.SetResource(fields, two));
  expected = zeros;
  expected[0] = 0xab;
  expected[1] = 0xcd;
  expected[10] = 0x7f;
  EXPECT_EQ(expected, Through(h, u, zeros)) << "the Init field was replaced";
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &two}}));

  EXPECT_NE("", h.SetResource(fields, Fields({{.offset = 0, .size = 1, .value = 1},
                                              {.offset = 0, .size = 9, .value = 1}})));
  EXPECT_NE("", h.SetResource(fields, Fields({{.offset = 4096, .size = 1, .value = 1}})));
  bess::pb::UpdateArg too_many;
  for (int i = 0; i < 17; i++) {
    too_many.MergeFrom(Fields({{.offset = i, .size = 1, .value = 1}}));
  }
  EXPECT_NE("", h.SetResource(fields, too_many));
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &two}})) << "refused sets change nothing";
  EXPECT_EQ(expected, Through(h, u, zeros));

  const bess::pb::UpdateArg none;
  EXPECT_EQ("", h.SetResource(fields, none));
  EXPECT_EQ(zeros, Through(h, u, zeros));
  EXPECT_EQ("", h.ReadBackDiffers(fields, {{nullptr, &none}}));
}

// The same fields must produce the same bytes through either configuration
// path. Refused replacements must leave the last accepted fields active.
TEST(UpdateTest, InitAndResourceAgreeOnBehaviorAndValidation) {
  ModuleHarness h;
  const auto fields = Fields({{.offset = 0, .size = 2, .value = 0xabcd},
                              {.offset = 59, .size = 1, .value = 0x81}});
  Module *initialized = h.Create("Update", fields);
  Module *transactional = h.Create("Update", bess::pb::UpdateArg());
  ASSERT_NE(nullptr, initialized);
  ASSERT_NE(nullptr, transactional);
  const std::string resource = transactional->name() + "/fields";
  ASSERT_EQ("", h.SetResource(resource, fields));
  const Bytes input(60, 0x55);
  Bytes expected = input;
  expected[0] = 0xab;
  expected[1] = 0xcd;
  expected[59] = 0x81;
  EXPECT_EQ(expected, Through(h, initialized, input));
  EXPECT_EQ(expected, Through(h, transactional, input));

  bess::pb::UpdateArg too_many;
  for (int i = 0; i < 17; i++) {
    too_many.MergeFrom(Fields({{.offset = i, .size = 1, .value = 1}}));
  }
  const std::vector<bess::pb::UpdateArg> invalid = {
      too_many,
      Fields({{.offset = -1, .size = 1, .value = 1}}),
      Fields({{.offset = 0, .size = 1, .value = 0x142}}),
      Fields({{.offset = INT64_MAX, .size = 8, .value = 1}})};
  for (const auto &configuration : invalid) {
    SCOPED_TRACE(configuration.ShortDebugString());
    EXPECT_NE("", h.CreateError("Update", configuration));
    EXPECT_NE("", h.SetResource(resource, configuration));
    EXPECT_EQ(expected, Through(h, transactional, input));
  }
}

// A field writes its own bytes and nothing else, including at the last byte
// of the data room: the eight-byte load/store it replaces reached seven bytes
// past it. The frame is the same on the old code (the neighbours were rewritten
// with their old values), so the overrun itself is what ASan catches; this test
// pins the bytes the write must produce.
TEST(UpdateTest, AFieldWritesExactlyItsBytes) {
  ModuleHarness h;
  const Bytes full(bess::kDefaultPacketDataSize, 0);
  Module *edge = h.Create(
      "Update",
      Fields({{.offset = bess::kDefaultPacketDataSize - 1, .size = 1, .value = 0xAB}}),
      "upd_edge");
  ASSERT_NE(nullptr, edge);
  Bytes expected = full;
  expected[bess::kDefaultPacketDataSize - 1] = 0xAB;
  EXPECT_EQ(expected, Through(h, edge, full));

  Module *two =
      h.Create("Update", Fields({{.offset = 0, .size = 2, .value = 0xAABB}}), "upd_two");
  ASSERT_NE(nullptr, two);
  Bytes in(60, 0);
  in[0] = 0x11;
  in[1] = 0x22;
  in[2] = 0x33;
  Bytes out = in;
  out[0] = 0xAA;
  out[1] = 0xBB;
  EXPECT_EQ(out, Through(h, two, in)) << "the byte after the field is untouched";
}

// 0, 1, 2, ... as a frame of `n` bytes.
Bytes Counting(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  return b;
}

// A field ending at byte 24 needs a 24-byte first segment: a 23-byte frame
// is dropped, a 24-byte one is written. The old code wrote the 23-byte
// frame's last field byte past its data and forwarded it.
TEST(UpdateTest, AFrameShorterThanAFieldIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("Update",
                       Fields({{.offset = 2, .size = 1, .value = 0xEE},
                               {.offset = 20, .size = 4, .value = 0xAABBCCDD}}),
                       "upd");
  ASSERT_NE(nullptr, m);

  auto out = h.Run(m, 0, {Counting(23)}, {0});
  EXPECT_TRUE(out[0].empty()) << "a 23-byte frame has no byte 23 to write";

  Bytes expected = Counting(24);
  expected[2] = 0xEE;
  expected[20] = 0xAA;
  expected[21] = 0xBB;
  expected[22] = 0xCC;
  expected[23] = 0xDD;
  EXPECT_EQ(expected, Through(h, m, Counting(24)));

  // The bound follows the list "<name>/fields" sets.
  ASSERT_EQ("", h.SetResource("upd/fields", Fields({{.offset = 40, .size = 2, .value = 0x1234}})));
  out = h.Run(m, 0, {Counting(41)}, {0});
  EXPECT_TRUE(out[0].empty()) << "the new list's field ends at 42";
  expected = Counting(42);
  expected[40] = 0x12;
  expected[41] = 0x34;
  EXPECT_EQ(expected, Through(h, m, Counting(42)));
}

// A chain whose first segment ends inside the field is dropped (the field is
// never written across segments); one whose first segment holds it is
// written there and the rest of the chain is untouched. The old code wrote
// past the first segment's data and forwarded the chain with the field's
// bytes still the old ones.
TEST(UpdateTest, AChainSplitInsideAFieldIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("Update", Fields({{.offset = 20, .size = 4, .value = 0xAABBCCDD}}), "upd");
  ASSERT_NE(nullptr, m);
  const Bytes frame = Counting(60);

  for (size_t split : {21u, 22u, 23u}) {
    auto out = h.RunPackets(m, 0, {MakePacket(frame, split)}, m, {0});
    EXPECT_TRUE(out[0].empty()) << "first segment of " << split << " bytes";
  }

  Bytes expected = frame;
  expected[20] = 0xAA;
  expected[21] = 0xBB;
  expected[22] = 0xCC;
  expected[23] = 0xDD;
  for (size_t split : {24u, 40u}) {
    auto out = h.RunPackets(m, 0, {MakePacket(frame, split)}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "first segment of " << split << " bytes";
    EXPECT_EQ(expected, out[0][0]) << "first segment of " << split << " bytes";
  }
}

// A clone shares its payload: the write goes to a copy, and the payload's
// other holder still reads the frame it had (D-153). The old code wrote into
// the shared bytes, so the original changed with the clone.
TEST(UpdateTest, ASharedFrameIsCopiedBeforeTheWrite) {
  ModuleHarness h;
  Module *m = h.Create("Update", Fields({{.offset = 0, .size = 6, .value = 0x020000000003}}), "upd");
  ASSERT_NE(nullptr, m);
  const Bytes frame = Counting(60);
  Bytes expected = frame;
  const Bytes mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
  std::copy(mac.begin(), mac.end(), expected.begin());

  // A one-segment frame and a chain: each clone shares every segment.
  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(m, 0, {clone}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "split " << split;
    EXPECT_EQ(expected, out[0][0]) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

}  // namespace
