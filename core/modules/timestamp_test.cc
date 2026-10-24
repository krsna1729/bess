// SPDX-License-Identifier: BSD-3-Clause

// Timestamp's in-packet stamp (`offset`): a 4-byte marker and the 8-byte
// time, written into the packet's first segment. A packet the stamp cannot be
// placed in there is dropped (D-172); a shared one is copied first (D-153).

#include "modules/timestamp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

constexpr size_t kStampSize = Timestamp::kStampSize;

bess::pb::TimestampArg At(uint64_t offset) {
  bess::pb::TimestampArg arg;
  arg.set_offset(offset);
  return arg;
}

Bytes Frame(size_t len) {
  Bytes frame(len);
  for (size_t i = 0; i < len; i++) {
    frame[i] = static_cast<uint8_t>(i);
  }
  return frame;
}

// Whether `out` is `in` with a stamp at `offset` (grown to hold it).
::testing::AssertionResult Stamped(const Bytes &in, const Bytes &out, size_t offset) {
  if (out.size() != std::max(in.size(), offset + kStampSize)) {
    return ::testing::AssertionFailure() << "length " << out.size();
  }
  Timestamp::MarkerType marker;
  std::memcpy(&marker, out.data() + offset, sizeof(marker));
  if (marker != Timestamp::kMarker) {
    return ::testing::AssertionFailure() << "no marker at " << offset;
  }
  uint64_t time;
  std::memcpy(&time, out.data() + offset + sizeof(marker), sizeof(time));
  if (time == 0) {
    return ::testing::AssertionFailure() << "no time";
  }
  for (size_t i = 0; i < in.size(); i++) {
    if ((i < offset || i >= offset + kStampSize) && out[i] != in[i]) {
      return ::testing::AssertionFailure() << "byte " << i << " changed";
    }
  }
  return ::testing::AssertionSuccess();
}

// The offset must leave room for the stamp inside the packet data room (the
// default pool's 2048 bytes here). Before, any offset was taken, and one past
// 65535 was cut to 16 bits by the write.
TEST(TimestampTest, AnOffsetPastTheDataRoomIsRefused) {
  ModuleHarness h;
  EXPECT_EQ("", h.CreateError("Timestamp", At(2048 - kStampSize)));
  EXPECT_EQ(EINVAL, h.CreateFailure("Timestamp", At(2048 - kStampSize + 1)).code());
  EXPECT_EQ(EINVAL, h.CreateFailure("Timestamp", At(65536 + 42)).code());
}

// A stamp inside the first segment is written there; a one-segment packet
// that ends before the stamp's end grows to hold it.
TEST(TimestampTest, TheStampIsWrittenInTheFirstSegment) {
  ModuleHarness h;
  Module *ts = h.Create("Timestamp", At(42), "ts_fits");
  ASSERT_NE(nullptr, ts);
  const Bytes full = Frame(60), short_frame = Frame(50);
  auto out = h.Run(ts, 0, {full, short_frame}, {0})[0];
  ASSERT_EQ(2u, out.size());
  EXPECT_TRUE(Stamped(full, out[0], 42));
  EXPECT_TRUE(Stamped(short_frame, out[1], 42));

  // A chain whose first segment holds the stamp.
  out = h.RunPackets(ts, 0, {MakePacket(full, 54)}, ts, {0})[0];
  ASSERT_EQ(1u, out.size());
  EXPECT_TRUE(Stamped(full, out[0], 42));
}

// A chain whose first segment ends before the stamp's end is dropped: an
// append grows the last segment, not the first. Before, the append grew the
// tail by two bytes and the stamp's last two bytes went past the first
// segment's data.
TEST(TimestampTest, AChainShortInItsFirstSegmentIsDropped) {
  ModuleHarness h;
  Module *ts = h.Create("Timestamp", At(20), "ts_chain");
  ASSERT_NE(nullptr, ts);
  const Bytes frame = Frame(60);
  EXPECT_TRUE(h.RunPackets(ts, 0, {MakePacket(frame, 30)}, ts, {0})[0].empty());
}

// A clone's payload is its parent's: stamping either one leaves the other's
// bytes as they were (D-153). Before, the stamp was written into the shared
// buffer.
TEST(TimestampTest, ASharedPayloadIsCopiedFirst) {
  ModuleHarness h;
  Module *ts = h.Create("Timestamp", At(42), "ts_shared");
  ASSERT_NE(nullptr, ts);
  const Bytes frame = Frame(60);
  for (size_t split : {0u, 56u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(ts, 0, {clone}, ts, {0})[0];
    ASSERT_EQ(1u, out.size());
    EXPECT_TRUE(Stamped(frame, out[0], 42)) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the parent is untouched";
    bess::PacketFree(original);

    original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    out = h.RunPackets(ts, 0, {original}, ts, {0})[0];
    ASSERT_EQ(1u, out.size());
    EXPECT_TRUE(Stamped(frame, out[0], 42)) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(clone)) << "split " << split << ": the clone is untouched";
    bess::PacketFree(clone);
  }
}

// A 60-byte packet over an external buffer with kRoom bytes of payload room,
// then guard bytes it does not own. It alone holds the buffer, so Timestamp
// writes in place.
class TimestampRoomTest : public ::testing::Test {
 protected:
  static constexpr uint16_t kRoom = 100;
  static constexpr size_t kBufLen = RTE_PKTMBUF_HEADROOM + kRoom;
  static constexpr size_t kDataLen = 60;

  bess::PacketHandle External() {
    std::memcpy(buffer_.data() + RTE_PKTMBUF_HEADROOM, Frame(kDataLen).data(), kDataLen);
    shinfo_.free_cb = [](void *, void *) {};
    shinfo_.fcb_opaque = nullptr;
    rte_mbuf_ext_refcnt_set(&shinfo_, 1);
    bess::PacketHandle p =
        pool_.AllocExternal(buffer_.data(), RTE_BAD_IOVA, kBufLen, &shinfo_, kDataLen);
    EXPECT_NE(nullptr, p);
    return p;
  }

  // Bytes of the buffer past the packet's data that are no longer 0xAA.
  size_t WrittenPastTheData() const {
    size_t written = 0;
    for (size_t i = RTE_PKTMBUF_HEADROOM + kDataLen; i < buffer_.size(); i++) {
      written += buffer_[i] != 0xAA ? 1 : 0;
    }
    return written;
  }

  ModuleHarness h_;
  bess::PlainPacketPool pool_{512};
  std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kBufLen + 256, 0xAA);
  rte_mbuf_ext_shared_info shinfo_{};
};

// A stamp that starts past the packet's data is not written and the packet is
// dropped (growing it would put the stale bytes between its end and the
// stamp on the wire; forwarding it would let a reader take whatever the
// buffer holds at the offset for a stamp). Before, data_len - offset wrapped,
// no append was made and the stamp landed past the data -- here past the
// buffer's end.
TEST_F(TimestampRoomTest, AnOffsetPastTheDataIsDropped) {
  Module *ts = h_.Create("Timestamp", At(kRoom + 20), "ts_past");
  ASSERT_NE(nullptr, ts);
  EXPECT_TRUE(h_.RunPackets(ts, 0, {External()}, ts, {0})[0].empty());
  EXPECT_EQ(0u, WrittenPastTheData());
}

// A stamp that starts inside the data but whose end the buffer cannot hold
// is not written either (the append fails): the packet is dropped.
TEST_F(TimestampRoomTest, AStampTheBufferCannotHoldDropsThePacket) {
  Module *ts = h_.Create("Timestamp", At(kDataLen - 4), "ts_full");
  ASSERT_NE(nullptr, ts);
  const Bytes in = Frame(kDataLen);
  auto out = h_.RunPackets(ts, 0, {External()}, ts, {0})[0];
  ASSERT_EQ(1u, out.size());
  EXPECT_TRUE(Stamped(in, out[0], kDataLen - 4)) << "the room holds it";

  Module *ts2 = h_.Create("Timestamp", At(kRoom - 4), "ts_over");
  ASSERT_NE(nullptr, ts2);
  std::fill(buffer_.begin(), buffer_.end(), 0xAA);
  // A packet whose data reaches the stamp's start, in a buffer without room
  // for its end.
  bess::PacketHandle p = External();
  ASSERT_NE(nullptr, bess::PacketRef(p).append(kRoom - kDataLen));
  std::memset(bess::PacketRef(p).head_data<uint8_t *>(kDataLen), 0x11, kRoom - kDataLen);
  EXPECT_TRUE(h_.RunPackets(ts2, 0, {p}, ts2, {0})[0].empty());
  size_t past = 0;
  for (size_t i = kBufLen; i < buffer_.size(); i++) {
    past += buffer_[i] != 0xAA ? 1 : 0;
  }
  EXPECT_EQ(0u, past) << "bytes written past the buffer";
}

}  // namespace
