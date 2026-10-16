// SPDX-License-Identifier: BSD-3-Clause

// RandomUpdate in process: each field overwrites `size` bytes at `offset`
// with a random value in [min, max] (min == max here, so the packets are
// known). D-129: "<name>/fields" holds the whole field list (setting it
// replaces the list); `add` and `clear` went with the legacy service (D-140).

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;

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

}  // namespace
