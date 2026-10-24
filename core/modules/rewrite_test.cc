// SPDX-License-Identifier: BSD-3-Clause

// Rewrite in process: each packet's data is replaced by the next template,
// round robin. D-129: "<name>/templates" holds the whole template list
// (setting it replaces the list and restarts the round robin); `add` and
// `clear` went with the legacy service (D-140).

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

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

  const bess::pb::RewriteArg none;
  EXPECT_EQ("", h.SetResource(templates, none));
  EXPECT_EQ((std::vector<Bytes>{pkt}), h.Run(rw, 0, {pkt}, {0})[0]) << "no template: unchanged";
  EXPECT_EQ("", h.ReadBackDiffers(templates, {{nullptr, &none}}));
}

TEST(RewriteTest, WorkersAdvanceIndependentlyAndAReplacementRestartsEveryWorker) {
  ModuleHarness h;
  const auto initial = Templates({"a", "b", "c"});
  Module *rw = h.Create("Rewrite", initial, "rw_workers");
  ASSERT_NE(nullptr, rw);
  const Bytes pkt(60, 0);
  auto run = [&](uint16_t worker) {
    h.SetWorker(bess::dataplane::WorkerId(worker));
    return h.Run(rw, 0, {pkt}, {0})[0];
  };
  EXPECT_EQ((std::vector<Bytes>{B("a")}), run(0));
  EXPECT_EQ((std::vector<Bytes>{B("a")}), run(1));
  EXPECT_EQ((std::vector<Bytes>{B("b")}), run(0));
  EXPECT_EQ((std::vector<Bytes>{B("b")}), run(1));

  // Both cursors are at 2: keeping them would emit "z", not the first template.
  ASSERT_EQ("", h.SetResource("rw_workers/templates", Templates({"x", "y", "z"})));
  EXPECT_EQ((std::vector<Bytes>{B("x")}), run(1));
  EXPECT_EQ((std::vector<Bytes>{B("x")}), run(0));
  EXPECT_EQ((std::vector<Bytes>{B("y")}), run(1));
  EXPECT_EQ((std::vector<Bytes>{B("y")}), run(0));
}

// A clone's payload is its parent's: rewriting either one leaves the other's
// bytes as they were, and every reference is released (D-153). One segment
// and a chain. Before, DPDK's reset cleared the clone's attachment flag, the
// template was written into the parent's buffer, and freeing the clone never
// released the parent; rewriting the parent wrote into the clone's bytes.
TEST(RewriteTest, ASharedPayloadIsNotWrittenThrough) {
  ModuleHarness h;
  const std::string t(60, 't');
  Module *rw = h.Create("Rewrite", Templates({t}), "rw_shared");
  ASSERT_NE(nullptr, rw);
  Bytes frame(100);
  for (size_t i = 0; i < frame.size(); i++) {
    frame[i] = static_cast<uint8_t>(i);
  }

  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(rw, 0, {clone}, rw, {0});
    EXPECT_EQ((std::vector<Bytes>{B(t)}), out[0]) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the parent is untouched";
    EXPECT_EQ(1, rte_mbuf_refcnt_read(original))
        << "split " << split << ": the clone's reference is released";
    bess::PacketFree(original);

    original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    out = h.RunPackets(rw, 0, {original}, rw, {0});
    EXPECT_EQ((std::vector<Bytes>{B(t)}), out[0]) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(clone)) << "split " << split << ": the clone is untouched";
    bess::PacketFree(clone);
  }

  // A chain held once: its tail is freed and the head rewritten.
  EXPECT_EQ((std::vector<Bytes>{B(t)}), h.RunPackets(rw, 0, {MakePacket(frame, 30)}, rw, {0})[0]);
}

// A packet over an external buffer: kRoom bytes of payload room, then guard
// bytes the packet does not own. It alone holds the buffer, so Rewrite writes
// in place.
class RewriteRoomTest : public ::testing::Test {
 protected:
  static constexpr uint16_t kRoom = 100;
  static constexpr size_t kBufLen = RTE_PKTMBUF_HEADROOM + kRoom;

  bess::PacketHandle External() {
    shinfo_.free_cb = [](void *, void *) {};
    shinfo_.fcb_opaque = nullptr;
    rte_mbuf_ext_refcnt_set(&shinfo_, 1);
    bess::PacketHandle p = pool_.AllocExternal(buffer_.data(), RTE_BAD_IOVA, kBufLen, &shinfo_, 60);
    EXPECT_NE(nullptr, p);
    return p;
  }

  ModuleHarness h_;
  bess::PlainPacketPool pool_{512};
  std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kBufLen + 64, 0xAA);
  rte_mbuf_ext_shared_info shinfo_{};
};

// The copy is exact: nothing past the template is written, inside the
// buffer or past it. Before, the copy moved whole 32-byte blocks: a 97-byte
// template wrote 128 bytes, 28 of them past the buffer's end.
TEST_F(RewriteRoomTest, NothingPastTheTemplateIsWritten) {
  const std::string fits(97, 'f');
  Module *rw = h_.Create("Rewrite", Templates({fits}), "rw_exact");
  ASSERT_NE(nullptr, rw);
  EXPECT_EQ((std::vector<Bytes>{B(fits)}), h_.RunPackets(rw, 0, {External()}, rw, {0})[0]);
  size_t written = 0;
  for (size_t i = RTE_PKTMBUF_HEADROOM + fits.size(); i < buffer_.size(); i++) {
    written += buffer_[i] != 0xAA ? 1 : 0;
  }
  EXPECT_EQ(0u, written) << "bytes written past the template";
}

// A template the packet's buffer cannot hold (a buffer smaller than the
// default pool's data room, which Init checks templates against) drops the
// packet. Before, the failed append returned NULL and the copy wrote through
// it.
TEST_F(RewriteRoomTest, ATemplateTheBufferCannotHoldDropsThePacket) {
  const std::string too_big(kRoom + 1, 'b');
  Module *rw = h_.Create("Rewrite", Templates({too_big}), "rw_too_big");
  ASSERT_NE(nullptr, rw);
  EXPECT_TRUE(h_.RunPackets(rw, 0, {External()}, rw, {0})[0].empty());
  EXPECT_EQ(std::string(60, '\xAA'),
            std::string(buffer_.begin() + RTE_PKTMBUF_HEADROOM,
                        buffer_.begin() + RTE_PKTMBUF_HEADROOM + 60))
      << "the buffer is untouched";
}

}  // namespace
