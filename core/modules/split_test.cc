// SPDX-License-Identifier: BSD-3-Clause

// Split in process, on a packet offset: the value is exactly the `size`
// bytes at `offset` of the first segment, a packet whose first segment ends
// before them is dropped (counted), and Init refuses an offset past 1024 or
// a value past the packet data room.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "module.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::MakeShortPacket;
using bess::testing::ModuleHarness;

bess::pb::SplitArg Arg(uint64_t size, int64_t offset) {
  bess::pb::SplitArg arg;
  arg.set_size(size);
  arg.set_offset(offset);
  return arg;
}

// `len` zero bytes with `value` at `at`.
Bytes Frame(size_t len, size_t at, std::initializer_list<uint8_t> value) {
  Bytes b(len, 0);
  size_t i = at;
  for (uint8_t v : value) {
    b[i++] = v;
  }
  return b;
}

// `pkt` through `m`: the gate (0-15) it left on, or -1 when it was dropped.
int GateOf(ModuleHarness &h, Module *m, bess::PacketHandle pkt) {
  const uint64_t dropped = m->deadends();
  for (const auto &[gate, out] : h.RunPackets(m, 0, {pkt}, m)) {
    if (!out.empty()) {
      EXPECT_EQ(1u, out.size());
      return gate;
    }
  }
  EXPECT_EQ(dropped + 1, m->deadends()) << "neither forwarded nor dropped";
  return -1;
}

// The value is the `size` bytes at `offset`, also when they are the last
// bytes of the frame: fewer than 8 bytes follow `offset` there.
TEST(SplitTest, TheValueIsTheSizeBytesAtTheOffset) {
  ModuleHarness h;
  Module *mid = h.Create("Split", Arg(2, 20));
  ASSERT_NE(nullptr, mid);
  EXPECT_EQ(1, GateOf(h, mid, MakePacket(Frame(60, 20, {0, 1}))));
  EXPECT_EQ(9, GateOf(h, mid, MakePacket(Frame(22, 20, {0, 9})))) << "the frame ends at the value";
  Module *last2 = h.Create("Split", Arg(2, 58));
  ASSERT_NE(nullptr, last2);
  EXPECT_EQ(3, GateOf(h, last2, MakePacket(Frame(60, 58, {0, 3}))));
  Module *last1 = h.Create("Split", Arg(1, 59));
  ASSERT_NE(nullptr, last1);
  EXPECT_EQ(4, GateOf(h, last1, MakePacket(Frame(60, 59, {4}))));
  Module *last8 = h.Create("Split", Arg(8, 52));
  ASSERT_NE(nullptr, last8);
  EXPECT_EQ(5, GateOf(h, last8, MakePacket(Frame(60, 52, {0, 0, 0, 0, 0, 0, 0, 5}))));
}

// A packet shorter than offset + size (here 22) has no value: it is dropped
// and counted, at every length from 1 to 21. The old code read the bytes
// past the data and forwarded each on gate 1.
TEST(SplitTest, AFrameShorterThanTheValueIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("Split", Arg(2, 20));
  ASSERT_NE(nullptr, m);
  const Bytes full = Frame(60, 20, {0, 1});
  for (size_t len = 1; len < 22; len++) {
    EXPECT_EQ(-1, GateOf(h, m, MakeShortPacket(full, len))) << len << " bytes";
  }
  EXPECT_EQ(1, GateOf(h, m, MakeShortPacket(full, 22)));
}

// Only the first segment counts (D-121): a chain whose first segment ends
// before or inside the value is dropped; one that holds it is split. The
// old code read past the first segment's data and forwarded each on gate 1.
TEST(SplitTest, AChainShortInItsFirstSegmentIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("Split", Arg(2, 20));
  ASSERT_NE(nullptr, m);
  const Bytes full = Frame(60, 20, {0, 1});
  for (size_t split : {10u, 20u, 21u}) {
    EXPECT_EQ(-1, GateOf(h, m, MakeShortPacket(full, split, true)))
        << "first segment of " << split << " bytes";
  }
  for (size_t split : {22u, 30u}) {
    EXPECT_EQ(1, GateOf(h, m, MakeShortPacket(full, split, true)))
        << "first segment of " << split << " bytes";
  }
}

// The largest offset (1024) with the largest value (8 bytes): a 1,032-byte
// frame is split by its last 8 bytes, a 1,031-byte one is dropped (the old
// code read past it). Offsets past 1024 and sizes outside 1-8 are refused.
TEST(SplitTest, TheOffsetAtItsLimit) {
  ModuleHarness h;
  Module *m = h.Create("Split", Arg(8, 1024));
  ASSERT_NE(nullptr, m);
  const Bytes full = Frame(1032, 1024, {0, 0, 0, 0, 0, 0, 0, 7});
  EXPECT_EQ(7, GateOf(h, m, MakePacket(full)));
  EXPECT_EQ(-1, GateOf(h, m, MakeShortPacket(full, 1031)));
  EXPECT_NE("", h.CreateError("Split", Arg(1, 1025)));
  EXPECT_NE("", h.CreateError("Split", Arg(1, -1)));
  EXPECT_NE("", h.CreateError("Split", Arg(0, 20)));
  EXPECT_NE("", h.CreateError("Split", Arg(9, 20)));
}

// Every packet leaves once: N in = N out + dropped.
TEST(SplitTest, EveryPacketIsForwardedOrDropped) {
  ModuleHarness h;
  Module *m = h.Create("Split", Arg(1, 20));
  ASSERT_NE(nullptr, m);
  const std::vector<Bytes> in = {Frame(60, 20, {1}), Frame(20, 0, {}), Frame(21, 20, {2}),
                                 Frame(5, 0, {}), Frame(60, 20, {1})};
  size_t out = 0;
  for (const auto &[gate, pkts] : h.Run(m, 0, in)) {
    out += pkts.size();
  }
  EXPECT_EQ(3u, out);
  EXPECT_EQ(in.size(), out + m->deadends());
}

}  // namespace
