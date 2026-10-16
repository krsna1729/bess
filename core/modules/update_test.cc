// SPDX-License-Identifier: BSD-3-Clause

// Update in process: each field overwrites `size` bytes at `offset` with its
// value. D-129: "<name>/fields" holds the whole field list (setting it
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

}  // namespace
