// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "packet_mutation.h"
#include "packet_pool.h"

namespace {

using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::kPacketPrivateSize;
using bess::PlainPacketPool;
using bess::packet::AppendInPlace;
using bess::packet::MutationError;
using bess::packet::PayloadWriteability;
using bess::packet::PayloadWriteabilityOf;
using bess::packet::PrependInPlace;
using bess::packet::RemovePrefixInPlace;
using bess::packet::TrimSuffixInPlace;

std::vector<std::byte> Pattern(size_t size) {
  std::vector<std::byte> bytes(size);
  for (size_t i = 0; i < size; i++) {
    bytes[i] = static_cast<std::byte>((i * 29 + 7) & 0xff);
  }
  return bytes;
}

PacketHandle BuildChain(PlainPacketPool &pool,
                        std::span<const size_t> segment_lengths,
                        std::span<const std::byte> bytes) {
  PacketHandle head = nullptr;
  PacketHandle previous = nullptr;
  size_t offset = 0;

  for (size_t length : segment_lengths) {
    PacketHandle segment = pool.Alloc(length);
    if (segment == nullptr) {
      if (head != nullptr) {
        PacketFree(head);
      }
      return nullptr;
    }
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    previous = segment;
    if (length != 0) {
      std::memcpy(PacketRef(segment).head_data(), bytes.data() + offset,
                  length);
    }
    offset += length;
  }

  if (head == nullptr || offset != bytes.size() ||
      segment_lengths.size() > UINT16_MAX) {
    if (head != nullptr) {
      PacketFree(head);
    }
    return nullptr;
  }
  head->pkt_len = static_cast<uint32_t>(bytes.size());
  head->nb_segs = static_cast<uint16_t>(segment_lengths.size());
  return head;
}

struct ExternalBufferOwner {
  int *free_count;
};

void FreeExternalBuffer(void *address, void *opaque) {
  auto *owner = static_cast<ExternalBufferOwner *>(opaque);
  ++*owner->free_count;
  delete[] static_cast<unsigned char *>(address);
  delete owner;
}

PacketHandle MakeExternal(PlainPacketPool &pool, int *free_count,
                          rte_mbuf_ext_shared_info **shinfo_out,
                          size_t data_len) {
  auto *buffer = new unsigned char[4096];
  auto *owner = new ExternalBufferOwner{free_count};
  uint16_t buffer_len = 4096;
  auto *shinfo = rte_pktmbuf_ext_shinfo_init_helper(
      buffer, &buffer_len, FreeExternalBuffer, owner);
  if (shinfo == nullptr) {
    delete[] buffer;
    delete owner;
    return nullptr;
  }

  PacketHandle packet = pool.AllocExternal(
      buffer, RTE_BAD_IOVA, buffer_len, shinfo, data_len);
  if (packet == nullptr) {
    delete[] buffer;
    delete owner;
    return nullptr;
  }
  *shinfo_out = shinfo;
  std::fill_n(PacketRef(packet).head_data<std::byte *>(), data_len,
              static_cast<std::byte>(0x5a));
  return packet;
}

struct SegmentSnapshot {
  PacketHandle handle;
  PacketHandle next;
  uint16_t data_off;
  uint16_t data_len;
  uint16_t nb_segs;
  uint32_t pkt_len;
  std::vector<std::byte> payload;
  std::vector<std::byte> metadata;
};

struct PacketSnapshot {
  std::vector<SegmentSnapshot> segments;
};

PacketSnapshot TakeSnapshot(PacketHandle packet) {
  PacketSnapshot snapshot;
  for (PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    SegmentSnapshot segment_snapshot = {
        segment,
        segment->next,
        segment->data_off,
        segment->data_len,
        segment->nb_segs,
        segment->pkt_len,
        std::vector<std::byte>(static_cast<size_t>(segment->data_len)),
        std::vector<std::byte>(kPacketPrivateSize),
    };
    if (!segment_snapshot.payload.empty()) {
      std::memcpy(segment_snapshot.payload.data(),
                  PacketRef(segment).head_data(),
                  segment_snapshot.payload.size());
    }
    std::memcpy(segment_snapshot.metadata.data(), rte_mbuf_to_priv(segment),
                segment_snapshot.metadata.size());
    snapshot.segments.push_back(std::move(segment_snapshot));
  }
  return snapshot;
}

void ExpectUnchanged(PacketHandle packet, const PacketSnapshot &before) {
  PacketSnapshot after = TakeSnapshot(packet);
  ASSERT_EQ(after.segments.size(), before.segments.size());
  for (size_t i = 0; i < before.segments.size(); i++) {
    const auto &expected = before.segments[i];
    const auto &actual = after.segments[i];
    EXPECT_EQ(actual.handle, expected.handle);
    EXPECT_EQ(actual.next, expected.next);
    EXPECT_EQ(actual.data_off, expected.data_off);
    EXPECT_EQ(actual.data_len, expected.data_len);
    EXPECT_EQ(actual.nb_segs, expected.nb_segs);
    EXPECT_EQ(actual.pkt_len, expected.pkt_len);
    EXPECT_EQ(actual.payload, expected.payload);
    EXPECT_EQ(actual.metadata, expected.metadata);
  }
}

TEST(PacketMutationTest, NullPacketsReturnExplicitError) {
  const PacketRef null_packet;

  auto prepend = PrependInPlace(null_packet, 0);
  ASSERT_FALSE(prepend.has_value());
  EXPECT_EQ(prepend.error(), MutationError::kNullPacket);

  auto append = AppendInPlace(null_packet, 0);
  ASSERT_FALSE(append.has_value());
  EXPECT_EQ(append.error(), MutationError::kNullPacket);

  auto remove = RemovePrefixInPlace(null_packet, 0);
  ASSERT_FALSE(remove.has_value());
  EXPECT_EQ(remove.error(), MutationError::kNullPacket);

  auto trim = TrimSuffixInPlace(null_packet, 0);
  ASSERT_FALSE(trim.has_value());
  EXPECT_EQ(trim.error(), MutationError::kNullPacket);
}

TEST(PacketMutationTest, UniqueDirectMutationsReturnWritableSpans) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> original = Pattern(16);
  PacketHandle packet = pool.AllocCopy(original.data(), original.size());
  ASSERT_NE(packet, nullptr);
  PacketRef ref(packet);

  EXPECT_EQ(PayloadWriteabilityOf(ref), PayloadWriteability::kWritable);
  const uint16_t initial_data_off = packet->data_off;

  auto prepended = PrependInPlace(ref, 8);
  ASSERT_TRUE(prepended.has_value());
  ASSERT_EQ(prepended->size(), 8u);
  std::fill(prepended->begin(), prepended->end(),
            static_cast<std::byte>(0xa1));
  EXPECT_EQ(packet->data_len, 24);
  EXPECT_EQ(packet->pkt_len, 24u);
  for (size_t i = 0; i < 8; i++) {
    EXPECT_EQ(ref.head_data<std::byte *>()[i], static_cast<std::byte>(0xa1));
  }
  EXPECT_EQ(std::memcmp(ref.head_data<const std::byte *>() + 8,
                        original.data(), original.size()),
            0);

  auto appended = AppendInPlace(ref, 12);
  ASSERT_TRUE(appended.has_value());
  ASSERT_EQ(appended->size(), 12u);
  std::fill(appended->begin(), appended->end(),
            static_cast<std::byte>(0xb2));
  EXPECT_EQ(packet->data_len, 36);
  EXPECT_EQ(packet->pkt_len, 36u);
  for (size_t i = 0; i < 12; i++) {
    EXPECT_EQ(ref.head_data<std::byte *>()[24 + i],
              static_cast<std::byte>(0xb2));
  }

  ASSERT_TRUE(RemovePrefixInPlace(ref, 8).has_value());
  ASSERT_TRUE(TrimSuffixInPlace(ref, 12).has_value());
  EXPECT_EQ(packet->data_off, initial_data_off);
  EXPECT_EQ(packet->data_len, original.size());
  EXPECT_EQ(packet->pkt_len, original.size());
  EXPECT_EQ(std::memcmp(ref.head_data(), original.data(), original.size()), 0);

  PacketFree(packet);
}

TEST(PacketMutationTest, SharedDirectAndIndirectPayloadRejectWrites) {
  PlainPacketPool pool(8, -1, 128);
  PacketHandle source = pool.Alloc(32);
  ASSERT_NE(source, nullptr);
  std::fill_n(PacketRef(source).head_data<std::byte *>(), 32,
              static_cast<std::byte>(0x3c));
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);

  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(source)),
            PayloadWriteability::kShared);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(clone)),
            PayloadWriteability::kShared);

  const PacketSnapshot source_before = TakeSnapshot(source);
  const PacketSnapshot clone_before = TakeSnapshot(clone);
  auto source_prepend = PrependInPlace(PacketRef(source), 4);
  ASSERT_FALSE(source_prepend.has_value());
  EXPECT_EQ(source_prepend.error(), MutationError::kSharedStorage);
  ExpectUnchanged(source, source_before);
  auto clone_append = AppendInPlace(PacketRef(clone), 4);
  ASSERT_FALSE(clone_append.has_value());
  EXPECT_EQ(clone_append.error(), MutationError::kSharedStorage);
  ExpectUnchanged(clone, clone_before);

  ASSERT_TRUE(RemovePrefixInPlace(PacketRef(source), 4).has_value());
  ASSERT_TRUE(TrimSuffixInPlace(PacketRef(clone), 4).has_value());
  EXPECT_EQ(source->pkt_len, 28u);
  EXPECT_EQ(clone->pkt_len, 28u);

  PacketFree(clone);
  PacketFree(source);
}

TEST(PacketMutationTest, IndirectPayloadBecomesWritableAfterBackingRelease) {
  PlainPacketPool pool(8, -1, 128);
  PacketHandle source = pool.Alloc(32);
  ASSERT_NE(source, nullptr);
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  PacketFree(source);

  EXPECT_FALSE(RTE_MBUF_DIRECT(clone));
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(clone)),
            PayloadWriteability::kWritable);
  auto prepended = PrependInPlace(PacketRef(clone), 4);
  ASSERT_TRUE(prepended.has_value());
  std::fill(prepended->begin(), prepended->end(),
            static_cast<std::byte>(0xd4));
  EXPECT_EQ(clone->pkt_len, 36u);
  ASSERT_TRUE(RemovePrefixInPlace(PacketRef(clone), 4).has_value());
  EXPECT_EQ(clone->pkt_len, 32u);

  PacketFree(clone);
}

TEST(PacketMutationTest, ExternalPayloadWriteabilityTracksShinfoReferences) {
  PlainPacketPool pool(8, -1, 128);

  int unique_free_count = 0;
  rte_mbuf_ext_shared_info *unique_shinfo = nullptr;
  PacketHandle unique =
      MakeExternal(pool, &unique_free_count, &unique_shinfo, 32);
  ASSERT_NE(unique, nullptr);
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(unique_shinfo), 1);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(unique)),
            PayloadWriteability::kWritable);
  auto unique_append = AppendInPlace(PacketRef(unique), 8);
  ASSERT_TRUE(unique_append.has_value());
  std::fill(unique_append->begin(), unique_append->end(),
            static_cast<std::byte>(0xe5));
  PacketFree(unique);
  EXPECT_EQ(unique_free_count, 1);

  int shared_free_count = 0;
  rte_mbuf_ext_shared_info *shared_shinfo = nullptr;
  PacketHandle external =
      MakeExternal(pool, &shared_free_count, &shared_shinfo, 32);
  ASSERT_NE(external, nullptr);
  PacketHandle clone = bess::PacketClone(external);
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shared_shinfo), 2);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(external)),
            PayloadWriteability::kShared);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(clone)),
            PayloadWriteability::kShared);

  const PacketSnapshot before = TakeSnapshot(external);
  auto append = AppendInPlace(PacketRef(external), 8);
  ASSERT_FALSE(append.has_value());
  EXPECT_EQ(append.error(), MutationError::kSharedStorage);
  ExpectUnchanged(external, before);

  PacketFree(clone);
  PacketFree(external);
  EXPECT_EQ(shared_free_count, 1);
}

TEST(PacketMutationTest, EdgeLengthsAndZeroOperationsUseExplicitErrors) {
  PlainPacketPool pool(16, -1, 64);
  PacketHandle packet = pool.Alloc();
  ASSERT_NE(packet, nullptr);
  const PacketSnapshot empty_before = TakeSnapshot(packet);

  ASSERT_TRUE(PrependInPlace(PacketRef(packet), 0).has_value());
  ASSERT_TRUE(AppendInPlace(PacketRef(packet), 0).has_value());
  ASSERT_TRUE(RemovePrefixInPlace(PacketRef(packet), 0).has_value());
  ASSERT_TRUE(TrimSuffixInPlace(PacketRef(packet), 0).has_value());
  ExpectUnchanged(packet, empty_before);

  auto maximum = AppendInPlace(
      PacketRef(packet), std::numeric_limits<uint16_t>::max());
  ASSERT_FALSE(maximum.has_value());
  EXPECT_EQ(maximum.error(), MutationError::kInsufficientTailroom);
  const PacketSnapshot wide_before = TakeSnapshot(packet);
  auto too_wide = AppendInPlace(
      PacketRef(packet), static_cast<size_t>(UINT16_MAX) + 1);
  ASSERT_FALSE(too_wide.has_value());
  EXPECT_EQ(too_wide.error(), MutationError::kLengthOutOfRange);
  ExpectUnchanged(packet, wide_before);

  packet->pkt_len = std::numeric_limits<uint32_t>::max();
  const PacketSnapshot overflow_before = TakeSnapshot(packet);
  auto overflow = AppendInPlace(PacketRef(packet), 1);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error(), MutationError::kLengthOutOfRange);
  ExpectUnchanged(packet, overflow_before);
  auto wide_remove = RemovePrefixInPlace(
      PacketRef(packet), static_cast<size_t>(UINT16_MAX) + 1);
  ASSERT_FALSE(wide_remove.has_value());
  EXPECT_EQ(wide_remove.error(), MutationError::kLengthOutOfRange);
  auto wide_trim = TrimSuffixInPlace(
      PacketRef(packet), static_cast<size_t>(UINT16_MAX) + 1);
  ASSERT_FALSE(wide_trim.has_value());
  EXPECT_EQ(wide_trim.error(), MutationError::kLengthOutOfRange);
  ExpectUnchanged(packet, overflow_before);
  packet->pkt_len = 0;

  auto out_of_range_prefix = RemovePrefixInPlace(PacketRef(packet), 1);
  ASSERT_FALSE(out_of_range_prefix.has_value());
  EXPECT_EQ(out_of_range_prefix.error(), MutationError::kLengthOutOfRange);
  auto out_of_range_suffix = TrimSuffixInPlace(PacketRef(packet), 1);
  ASSERT_FALSE(out_of_range_suffix.has_value());
  EXPECT_EQ(out_of_range_suffix.error(), MutationError::kLengthOutOfRange);

  PacketFree(packet);

  PacketHandle exact_append = pool.Alloc();
  ASSERT_NE(exact_append, nullptr);
  ASSERT_TRUE(AppendInPlace(PacketRef(exact_append), 64).has_value());
  const PacketSnapshot append_before = TakeSnapshot(exact_append);
  auto append_short = AppendInPlace(PacketRef(exact_append), 1);
  ASSERT_FALSE(append_short.has_value());
  EXPECT_EQ(append_short.error(), MutationError::kInsufficientTailroom);
  ExpectUnchanged(exact_append, append_before);
  PacketFree(exact_append);

  PacketHandle exact_prepend = pool.Alloc();
  ASSERT_NE(exact_prepend, nullptr);
  ASSERT_TRUE(PrependInPlace(PacketRef(exact_prepend), RTE_PKTMBUF_HEADROOM)
                  .has_value());
  const PacketSnapshot prepend_before = TakeSnapshot(exact_prepend);
  auto prepend_short = PrependInPlace(PacketRef(exact_prepend), 1);
  ASSERT_FALSE(prepend_short.has_value());
  EXPECT_EQ(prepend_short.error(), MutationError::kInsufficientHeadroom);
  ExpectUnchanged(exact_prepend, prepend_before);
  PacketFree(exact_prepend);
}

TEST(PacketMutationTest, CrossSegmentRemovalReportsTopologyBoundary) {
  PlainPacketPool pool(16, -1, 64);
  const std::vector<std::byte> bytes = Pattern(64);
  const std::array<size_t, 2> lengths = {32, 32};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);

  const PacketHandle first = packet;
  const PacketHandle second = packet->next;
  const PacketSnapshot before = TakeSnapshot(packet);
  auto prefix = RemovePrefixInPlace(PacketRef(packet), 33);
  ASSERT_FALSE(prefix.has_value());
  EXPECT_EQ(prefix.error(), MutationError::kCrossesSegment);
  ExpectUnchanged(packet, before);
  auto suffix = TrimSuffixInPlace(PacketRef(packet), 33);
  ASSERT_FALSE(suffix.has_value());
  EXPECT_EQ(suffix.error(), MutationError::kCrossesSegment);
  ExpectUnchanged(packet, before);

  auto prepended = PrependInPlace(PacketRef(packet), 8);
  ASSERT_TRUE(prepended.has_value());
  auto appended = AppendInPlace(PacketRef(packet), 8);
  ASSERT_TRUE(appended.has_value());
  std::fill(prepended->begin(), prepended->end(),
            static_cast<std::byte>(0x71));
  std::fill(appended->begin(), appended->end(),
            static_cast<std::byte>(0x82));
  EXPECT_EQ(std::memcmp(PacketRef(packet).head_data<const std::byte *>() + 8,
                        bytes.data(), bytes.size() / 2),
            0);
  EXPECT_EQ(std::memcmp(PacketRef(second).head_data<const std::byte *>(),
                        bytes.data() + bytes.size() / 2,
                        bytes.size() / 2),
            0);
  EXPECT_TRUE(std::all_of(
      prepended->begin(), prepended->end(),
      [](std::byte value) { return value == static_cast<std::byte>(0x71); }));
  EXPECT_TRUE(std::all_of(
      appended->begin(), appended->end(),
      [](std::byte value) { return value == static_cast<std::byte>(0x82); }));
  EXPECT_EQ(packet, first);
  EXPECT_EQ(packet->next, second);
  EXPECT_EQ(packet->nb_segs, 2);
  EXPECT_EQ(packet->data_len, 40);
  EXPECT_EQ(packet->next->data_len, 40);
  EXPECT_EQ(packet->pkt_len, 80u);

  ASSERT_TRUE(RemovePrefixInPlace(PacketRef(packet), 40).has_value());
  ASSERT_TRUE(TrimSuffixInPlace(PacketRef(packet), 40).has_value());
  EXPECT_EQ(packet, first);
  EXPECT_EQ(packet->next, second);
  EXPECT_EQ(packet->nb_segs, 2);
  EXPECT_EQ(packet->pkt_len, 0u);
  EXPECT_EQ(packet->data_len, 0);
  EXPECT_EQ(packet->next->data_len, 0);

  PacketFree(packet);
}

TEST(PacketMutationTest, FailedOperationsPreserveAllPacketState) {
  PlainPacketPool pool(16, -1, 64);

  PacketHandle packet = pool.Alloc();
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(rte_pktmbuf_prepend(packet, RTE_PKTMBUF_HEADROOM - 1), nullptr);
  PacketSnapshot before = TakeSnapshot(packet);
  auto prepend = PrependInPlace(PacketRef(packet), 2);
  ASSERT_FALSE(prepend.has_value());
  EXPECT_EQ(prepend.error(), MutationError::kInsufficientHeadroom);
  ExpectUnchanged(packet, before);
  PacketFree(packet);

  packet = pool.Alloc();
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(rte_pktmbuf_append(packet, 63), nullptr);
  before = TakeSnapshot(packet);
  auto append = AppendInPlace(PacketRef(packet), 2);
  ASSERT_FALSE(append.has_value());
  EXPECT_EQ(append.error(), MutationError::kInsufficientTailroom);
  ExpectUnchanged(packet, before);
  PacketFree(packet);

  const std::vector<std::byte> bytes = Pattern(64);
  const std::array<size_t, 2> lengths = {32, 32};
  packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);
  before = TakeSnapshot(packet);
  auto cross_prefix = RemovePrefixInPlace(PacketRef(packet), 33);
  ASSERT_FALSE(cross_prefix.has_value());
  EXPECT_EQ(cross_prefix.error(), MutationError::kCrossesSegment);
  ExpectUnchanged(packet, before);
  auto cross_suffix = TrimSuffixInPlace(PacketRef(packet), 33);
  ASSERT_FALSE(cross_suffix.has_value());
  EXPECT_EQ(cross_suffix.error(), MutationError::kCrossesSegment);
  ExpectUnchanged(packet, before);
  PacketFree(packet);
}

}  // namespace
