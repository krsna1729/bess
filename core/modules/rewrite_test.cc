// SPDX-License-Identifier: BSD-3-Clause

// Rewrite in process: each packet's data is replaced by the next template,
// round robin. D-129: "<name>/templates" holds the whole template list
// (setting it replaces the list and restarts the round robin), and add/clear
// are legacy-only.

#include <gtest/gtest.h>

#include <cerrno>
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

bess::pb::RewriteArg Templates(std::initializer_list<std::string> templates) {
  bess::pb::RewriteArg arg;
  for (const std::string &t : templates) {
    arg.add_templates(t);
  }
  return arg;
}

Bytes B(const std::string &s) { return Bytes(s.begin(), s.end()); }

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

TEST(RewriteTest, TemplatesResourceReplacesTheList) {
  const std::string t0(60, 'i');
  const std::string t1(60, 'a');
  const std::string t2(64, 'b');
  const Bytes pkt(60, 0);

  ModuleHarness h;
  const auto initial = Templates({t0});
  Module *rw = h.Create("Rewrite", initial, "rw_res");
  ASSERT_NE(nullptr, rw);
  const std::string templates = "rw_res/templates";
  EXPECT_TRUE(RequiresPausedWorkers(templates));
  EXPECT_EQ("", h.ReadBackDiffers(templates, {{nullptr, &initial}})) << "Init's templates";
  EXPECT_EQ((std::vector<Bytes>{B(t0), B(t0)}), h.Run(rw, 0, {pkt, pkt}, {0})[0]);

  const auto two = Templates({t1, t2});
  EXPECT_EQ("", h.SetResource(templates, two));
  EXPECT_EQ((std::vector<Bytes>{B(t1), B(t2), B(t1)}), h.Run(rw, 0, {pkt, pkt, pkt}, {0})[0]);
  EXPECT_EQ((std::vector<Bytes>{B(t2)}), h.Run(rw, 0, {pkt}, {0})[0]) << "round robin goes on";
  EXPECT_EQ("", h.ReadBackDiffers(templates, {{nullptr, &two}}));

  EXPECT_NE("", h.SetResource(templates, Templates({t1, std::string(1537, 'x')})));
  bess::pb::RewriteArg too_many;
  for (int i = 0; i < 33; i++) {
    too_many.add_templates(t1);
  }
  EXPECT_NE("", h.SetResource(templates, too_many));
  EXPECT_EQ("", h.ReadBackDiffers(templates, {{nullptr, &two}})) << "refused sets change nothing";

  // A set restarts the round robin at the first template.
  EXPECT_EQ("", h.SetResource(templates, two));
  EXPECT_EQ((std::vector<Bytes>{B(t1)}), h.Run(rw, 0, {pkt}, {0})[0]);

  // The legacy add appends to the same list.
  h.MustCommand(rw, "add", Templates({t0}));
  const auto three = Templates({t1, t2, t0});
  EXPECT_EQ("", h.ReadBackDiffers(templates, {{nullptr, &three}}));
  EXPECT_EQ((std::vector<Bytes>{B(t2), B(t0), B(t1)}), h.Run(rw, 0, {pkt, pkt, pkt}, {0})[0]);

  const bess::pb::RewriteArg none;
  EXPECT_EQ("", h.SetResource(templates, none));
  EXPECT_EQ((std::vector<Bytes>{pkt}), h.Run(rw, 0, {pkt}, {0})[0]) << "no template: unchanged";
  EXPECT_EQ("", h.ReadBackDiffers(templates, {{nullptr, &none}}));
}

// add and clear edit "<name>/templates": control_v2's InvokeAction refuses
// them.
TEST(RewriteTest, CommandsAreLegacyOnly) {
  ModuleHarness h;
  Module *rw = h.Create("Rewrite", bess::pb::RewriteArg());
  ASSERT_NE(nullptr, rw);
  const auto one = Templates({std::string(60, 'a')});
  EXPECT_EQ(ENOTSUP, h.Action(rw, "add", one).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(rw, "clear", bess::pb::EmptyArg()).error().code());
  const Bytes pkt(60, 0);
  EXPECT_EQ((std::vector<Bytes>{pkt}), h.Run(rw, 0, {pkt}, {0})[0]) << "the refused add did nothing";
  EXPECT_EQ(0, h.Command(rw, "add", one).error().code());
}

}  // namespace
