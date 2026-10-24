// SPDX-License-Identifier: BSD-3-Clause

// GenericEncap's header bytes: constants in configuration order, metadata
// fields copied from their attributes, every field exactly its size (a
// field never spills into the next one or into the packet).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/generic_encap.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "task.h"
#include "testing/module_harness.h"

namespace {

using bess::PacketBatch;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::pb::GenericEncapArg;
using bess::testing::Bytes;
using bess::testing::FillHeadroom;
using bess::testing::HeadroomBytes;
using bess::testing::MakePacket;
using bess::testing::MakePacketWithoutHeadroom;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

class GenericEncapTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  Module *Create(const std::string &cls, const std::string &name,
                 const google::protobuf::Message &arg) {
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(
        ModuleBuilder::all_module_builders().at(cls), name, packed, &perr);
    EXPECT_EQ(0, perr.code()) << perr.errmsg();
    return m;
  }

  // Creates a GenericEncap whose output gate 0 feeds a Sink.
  Module *CreateEncap(const GenericEncapArg &arg) {
    Module *ge = Create("GenericEncap", "ge", arg);
    Module *sink = Create("Sink", "sink", bess::pb::SinkArg());
    if (ge != nullptr && sink != nullptr) {
      EXPECT_EQ(0, ModuleGraph::ConnectModules(ge, 0, sink, 0));
      // The Track hook needs a worker thread; this test runs on none.
      ge->ogates()[0]->ClearHooks();
    }
    return ge;
  }

  // Runs `ge` over packets of `payload`, with `metadata` written at metadata
  // offset 0 of each; returns each packet's bytes after the module.
  std::vector<std::vector<uint8_t>> Run(Module *ge, size_t packets,
                                        const std::vector<uint8_t> &payload,
                                        const std::vector<uint8_t> &metadata) {
    PacketBatch batch;
    batch.clear();
    for (size_t i = 0; i < packets; i++) {
      bess::PacketHandle h = pool_.Alloc();
      EXPECT_NE(nullptr, h);
      PacketRef ref(h);
      std::memcpy(ref.append(payload.size()), payload.data(), payload.size());
      std::memcpy(ref.metadata<uint8_t *>(), metadata.data(), metadata.size());
      batch.add(ref);
    }
    Task task(ge, nullptr);
    Context ctx = {};
    ctx.task = &task;
    ge->ProcessBatch(&ctx, &batch);
    std::vector<std::vector<uint8_t>> out;
    for (PacketRef ref : batch.packets()) {
      const auto *p = ref.head_data<const uint8_t *>();
      out.emplace_back(p, p + ref.data_len());
    }
    bess::PacketFreeBatch(&batch);
    return out;
  }

  PlainPacketPool pool_{64};
};

void AddValue(GenericEncapArg &arg, uint64_t size, uint64_t value) {
  auto *f = arg.add_fields();
  f->set_size(size);
  f->mutable_value()->set_value_int(value);
}

void AddAttribute(GenericEncapArg &arg, uint64_t size, const std::string &name) {
  auto *f = arg.add_fields();
  f->set_size(size);
  f->set_attribute(name);
}

std::vector<uint8_t> Payload() {
  std::vector<uint8_t> p(40);
  for (size_t i = 0; i < p.size(); i++) {
    p[i] = static_cast<uint8_t>(0xc0 + i);
  }
  return p;
}

TEST_F(GenericEncapTest, ConstantsAndAttributesOfEverySizeInOrder) {
  // Sizes 3, 2, 1, 8, 5, 7, 4, 6: every field size, a metadata field before
  // a constant, and a metadata field last.
  GenericEncapArg arg;
  AddValue(arg, 3, 0xaabbcc);
  AddAttribute(arg, 2, "a");
  AddValue(arg, 1, 0x11);
  AddAttribute(arg, 8, "b");
  AddValue(arg, 5, 0x0102030405);
  AddAttribute(arg, 7, "c");
  AddValue(arg, 4, 0xdeadbeef);
  AddValue(arg, 6, 0x212223242526);
  Module *ge = CreateEncap(arg);
  ASSERT_NE(nullptr, ge);
  ge->set_attr_offset(0, 0);   // a
  ge->set_attr_offset(1, 8);   // b
  ge->set_attr_offset(2, 16);  // c

  std::vector<uint8_t> metadata(24);
  for (size_t i = 0; i < metadata.size(); i++) {
    metadata[i] = static_cast<uint8_t>(0x30 + i);
  }
  std::vector<uint8_t> want = {0xaa, 0xbb, 0xcc, 0x30, 0x31, 0x11};
  want.insert(want.end(), metadata.begin() + 8, metadata.begin() + 16);  // b
  want.insert(want.end(), {0x01, 0x02, 0x03, 0x04, 0x05});
  want.insert(want.end(), metadata.begin() + 16, metadata.begin() + 23);  // c
  want.insert(want.end(), {0xde, 0xad, 0xbe, 0xef, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26});
  ASSERT_EQ(36u, want.size());
  const std::vector<uint8_t> payload = Payload();
  want.insert(want.end(), payload.begin(), payload.end());

  // A full burst and a partial one.
  for (size_t n : {size_t{PacketBatch::kMaxBurst}, size_t{3}}) {
    const auto out = Run(ge, n, payload, metadata);
    ASSERT_EQ(n, out.size());
    for (const auto &bytes : out) {
      EXPECT_EQ(want, bytes);
    }
  }
}

TEST_F(GenericEncapTest, AnUnreadableAttributeIsZeros) {
  GenericEncapArg arg;
  AddValue(arg, 2, 0x0800);
  AddAttribute(arg, 4, "missing");
  AddValue(arg, 1, 0x7f);
  Module *ge = CreateEncap(arg);
  ASSERT_NE(nullptr, ge);
  // No upstream writer: the graph leaves the offset invalid.
  ge->set_attr_offset(0, bess::metadata::kMetadataOffsetNoRead);

  std::vector<uint8_t> want = {0x08, 0x00, 0, 0, 0, 0, 0x7f};
  const std::vector<uint8_t> payload = Payload();
  want.insert(want.end(), payload.begin(), payload.end());
  const auto out = Run(ge, 2, payload, std::vector<uint8_t>(8, 0xee));
  ASSERT_EQ(2u, out.size());
  for (const auto &bytes : out) {
    EXPECT_EQ(want, bytes);
  }
}

TEST_F(GenericEncapTest, MoreFieldsThanItHoldsIsAnError) {
  GenericEncapArg arg;
  for (int i = 0; i < MAX_FIELDS + 1; i++) {
    AddValue(arg, 1, static_cast<uint64_t>(i));
  }
  google::protobuf::Any packed;
  ASSERT_TRUE(packed.PackFrom(arg));
  pb_error_t perr;
  EXPECT_EQ(nullptr, ModuleGraph::CreateModule(
                         ModuleBuilder::all_module_builders().at("GenericEncap"),
                         "ge", packed, &perr));
  EXPECT_EQ(EINVAL, perr.code());
}

// A 14-byte header of constants (an Ethernet header), for the tests below.
GenericEncapArg EthernetHeader() {
  GenericEncapArg arg;
  AddValue(arg, 6, 0x020000000002);
  AddValue(arg, 6, 0x020000000001);
  AddValue(arg, 2, 0x0800);
  return arg;
}

// `inner` behind EthernetHeader's bytes.
Bytes Wrapped(const Bytes &inner) {
  Bytes b = {0x02, 0, 0, 0, 0, 0x02, 0x02, 0, 0, 0, 0, 0x01, 0x08, 0x00};
  b.insert(b.end(), inner.begin(), inner.end());
  return b;
}

// A clone's headroom is its source's buffer, so the header goes into a copy,
// in one segment and in a chain: the other holder keeps its bytes and its
// headroom, whichever of the two is sent. The old code wrote the header into
// the shared headroom.
TEST(GenericEncapSafetyTest, ASharedPacketIsCopiedBeforeTheHeader) {
  ModuleHarness h;
  Module *ge = h.Create("GenericEncap", EthernetHeader());
  ASSERT_NE(nullptr, ge);
  const std::vector<uint8_t> in = Payload();
  for (bool send_clone : {true, false}) {
    for (size_t split : {0u, 20u}) {
      SCOPED_TRACE(::testing::Message() << "clone " << send_clone << ", split " << split);
      bess::PacketHandle original = MakePacket(in, split);
      ASSERT_NE(nullptr, original);
      FillHeadroom(original, 14, 0x11);
      bess::PacketHandle clone = bess::PacketClone(original);
      ASSERT_NE(nullptr, clone);
      auto out = h.RunPackets(ge, 0, {send_clone ? clone : original}, ge, {0});
      bess::PacketHandle kept = send_clone ? original : clone;
      ASSERT_EQ(1u, out[0].size());
      EXPECT_EQ(Wrapped(in), out[0][0]);
      EXPECT_EQ(in, PacketBytes(kept)) << "the other holder's bytes are untouched";
      EXPECT_EQ(Bytes(14, 0x11), HeadroomBytes(kept, 14))
          << "the other holder's headroom is untouched";
      bess::PacketFree(kept);
    }
  }
}

// Every packet leaves wrapped on gate 0 or is dropped: one segment, a chain,
// a clone, an empty packet (GenericEncap reads nothing of the packet, so an
// empty one is wrapped too) and one without headroom, which is dropped (the
// old code sent it on without the header). A shared packet without headroom
// is dropped too, not copied.
TEST(GenericEncapSafetyTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  Module *ge = h.Create("GenericEncap", EthernetHeader());
  ASSERT_NE(nullptr, ge);
  const std::vector<uint8_t> in = Payload();
  bess::PacketHandle source = MakePacket(in);
  ASSERT_NE(nullptr, source);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(nullptr, clone);
  auto out = h.RunPackets(ge, 0,
                          {MakePacket(in), MakePacket(in, 20), clone, MakePacket(Bytes()),
                           MakePacketWithoutHeadroom(in)},
                          ge, {0});
  EXPECT_EQ((std::vector<Bytes>{Wrapped(in), Wrapped(in), Wrapped(in), Wrapped(Bytes())}), out[0])
      << "5 in, the one without headroom dropped";
  EXPECT_EQ(in, PacketBytes(source));
  bess::PacketFree(source);

  bess::PacketHandle bare = MakePacketWithoutHeadroom(in);
  ASSERT_NE(nullptr, bare);
  bess::PacketHandle bare_clone = bess::PacketClone(bare);
  ASSERT_NE(nullptr, bare_clone);
  out = h.RunPackets(ge, 0, {bare_clone}, ge, {0});
  EXPECT_TRUE(out[0].empty()) << "a shared packet without headroom is dropped";
  EXPECT_EQ(in, PacketBytes(bare));
  bess::PacketFree(bare);
}

}  // namespace
