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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "packet_cursor.h"
#include "packet_pool.h"

namespace {

using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::packet::PacketCursor;

std::vector<std::byte> Pattern(size_t size) {
  std::vector<std::byte> bytes(size);
  for (size_t i = 0; i < size; i++) {
    bytes[i] = static_cast<std::byte>((i * 37 + 11) & 0xff);
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

bool ReadAll(PacketHandle packet, std::span<const std::byte> expected) {
  PacketCursor cursor{PacketRef(packet)};
  std::vector<std::byte> actual(expected.size());
  if (!cursor.ReadBytes(actual)) {
    return false;
  }
  return cursor.offset() == expected.size() && cursor.remaining() == 0 &&
         std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
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

TEST(PacketCursorTest, ContiguousPeekTypedReadAndCopyRestore) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> bytes = Pattern(64);
  const std::array<size_t, 1> lengths = {bytes.size()};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);

  PacketCursor cursor{PacketRef(packet)};
  EXPECT_EQ(cursor.offset(), 0u);
  EXPECT_EQ(cursor.remaining(), bytes.size());

  const auto peek = cursor.PeekContiguous(8);
  ASSERT_EQ(peek.size(), 8u);
  EXPECT_EQ(std::memcmp(peek.data(), bytes.data(), peek.size()), 0);
  EXPECT_EQ(cursor.offset(), 0u);

  std::array<std::byte, 8> first{};
  ASSERT_TRUE(cursor.ReadBytes(first));
  EXPECT_EQ(std::memcmp(first.data(), bytes.data(), first.size()), 0);

  const PacketCursor saved = cursor;
  uint32_t expected = 0;
  std::memcpy(&expected, bytes.data() + first.size(), sizeof(expected));
  const auto value = cursor.Read<uint32_t>();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, expected);
  EXPECT_EQ(cursor.offset(), 12u);

  cursor = saved;
  EXPECT_EQ(cursor.offset(), 8u);
  EXPECT_EQ(cursor.remaining(), bytes.size() - 8);
  EXPECT_EQ(cursor.PeekContiguous(4).size(), 4u);

  PacketFree(packet);
}

TEST(PacketCursorTest, ReadsEveryWidthAcrossEveryTwoSegmentBoundary) {
  constexpr std::array<size_t, 8> widths = {1, 2, 3, 4, 7, 8, 16, 32};
  for (size_t width : widths) {
    const std::vector<std::byte> bytes = Pattern(width);
    for (size_t split = 0; split <= width; split++) {
      PlainPacketPool pool(8, -1, 64);
      std::vector<size_t> lengths;
      if (split == 0) {
        lengths = {0, width};
      } else if (split == width) {
        lengths = {width, 0};
      } else {
        lengths = {split, width - split};
      }
      PacketHandle packet = BuildChain(pool, lengths, bytes);
      ASSERT_NE(packet, nullptr) << "width=" << width << " split=" << split;

      PacketCursor cursor{PacketRef(packet)};
      std::vector<std::byte> actual(width);
      ASSERT_TRUE(cursor.ReadBytes(actual))
          << "width=" << width << " split=" << split;
      EXPECT_EQ(std::memcmp(actual.data(), bytes.data(), width), 0)
          << "width=" << width << " split=" << split;
      EXPECT_EQ(cursor.offset(), width);
      EXPECT_EQ(cursor.remaining(), 0u);

      PacketFree(packet);
    }
  }
}

TEST(PacketCursorTest, ReadsAcrossThreeSegmentsAndSkipsBoundaries) {
  const std::vector<std::byte> bytes = Pattern(32);
  const std::array<size_t, 8> lengths = {1, 2, 3, 4, 5, 6, 7, 4};
  PlainPacketPool pool(16, -1, 64);
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);

  PacketCursor cursor{PacketRef(packet)};
  ASSERT_TRUE(cursor.Skip(14));
  EXPECT_EQ(cursor.offset(), 14u);
  ASSERT_EQ(cursor.PeekContiguous(1).size(), 1u);
  EXPECT_EQ(cursor.PeekContiguous(1)[0], bytes[14]);

  std::array<std::byte, 18> tail{};
  ASSERT_TRUE(cursor.ReadBytes(tail));
  EXPECT_EQ(std::memcmp(tail.data(), bytes.data() + 14, tail.size()), 0);
  EXPECT_EQ(cursor.offset(), bytes.size());
  EXPECT_EQ(cursor.remaining(), 0u);

  PacketFree(packet);
}

TEST(PacketCursorTest, FailedReadsAndSkipsAreTransactional) {
  const std::vector<std::byte> bytes = Pattern(6);
  const std::array<size_t, 2> lengths = {3, 3};
  PlainPacketPool pool(8, -1, 64);
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);

  PacketCursor cursor{PacketRef(packet)};
  std::array<std::byte, 7> too_large{};
  EXPECT_FALSE(cursor.ReadBytes(too_large));
  EXPECT_EQ(cursor.offset(), 0u);
  EXPECT_EQ(cursor.remaining(), bytes.size());
  EXPECT_FALSE(cursor.Skip(bytes.size() + 1));
  EXPECT_EQ(cursor.offset(), 0u);
  EXPECT_EQ(cursor.remaining(), bytes.size());

  std::array<std::byte, 6> all{};
  ASSERT_TRUE(cursor.ReadBytes(all));
  EXPECT_EQ(std::memcmp(all.data(), bytes.data(), all.size()), 0);
  EXPECT_FALSE(cursor.Skip(1));
  EXPECT_EQ(cursor.offset(), bytes.size());
  EXPECT_EQ(cursor.remaining(), 0u);
  EXPECT_TRUE(cursor.PeekContiguous(0).empty());

  PacketFree(packet);

  PlainPacketPool malformed_pool(4, -1, 64);
  const std::array<size_t, 1> short_lengths = {4};
  PacketHandle malformed = BuildChain(malformed_pool, short_lengths,
                                      std::span<const std::byte>(bytes).first(4));
  ASSERT_NE(malformed, nullptr);
  malformed->pkt_len = 8;
  PacketCursor malformed_cursor{PacketRef(malformed)};
  std::array<std::byte, 8> malformed_output{};
  EXPECT_FALSE(malformed_cursor.ReadBytes(malformed_output));
  EXPECT_EQ(malformed_cursor.offset(), 0u);
  EXPECT_EQ(malformed_cursor.remaining(), 8u);
  PacketFree(malformed);
}

TEST(PacketCursorTest, HandlesDirectExternalAndClonedStorage) {
  const std::vector<std::byte> bytes = Pattern(96);

  PlainPacketPool small_pool(16, -1, 32);
  PacketHandle direct = small_pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(direct, nullptr);
  EXPECT_TRUE(ReadAll(direct, bytes));
  PacketFree(direct);

  PlainPacketPool large_pool(16, -1, 128);
  PacketHandle source = large_pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  EXPECT_TRUE(ReadAll(source, bytes));
  EXPECT_TRUE(ReadAll(clone, bytes));
  PacketFree(clone);
  PacketFree(source);

  int free_count = 0;
  auto *buffer = new unsigned char[4096];
  auto *owner = new ExternalBufferOwner{&free_count};
  uint16_t buffer_len = 4096;
  auto *shinfo = rte_pktmbuf_ext_shinfo_init_helper(
      buffer, &buffer_len, FreeExternalBuffer, owner);
  ASSERT_NE(shinfo, nullptr);
  std::memcpy(buffer + RTE_PKTMBUF_HEADROOM, bytes.data(), bytes.size());
  PacketHandle external = large_pool.AllocExternal(
      buffer, RTE_BAD_IOVA, buffer_len, shinfo, bytes.size());
  ASSERT_NE(external, nullptr);
  EXPECT_TRUE(ReadAll(external, bytes));
  PacketFree(external);
  EXPECT_EQ(free_count, 1);
}

TEST(PacketCursorTest, NullPacketIsEmptyAndSafe) {
  PacketCursor cursor{PacketRef()};
  EXPECT_EQ(cursor.offset(), 0u);
  EXPECT_EQ(cursor.remaining(), 0u);
  EXPECT_FALSE(cursor.Skip(1));
  EXPECT_TRUE(cursor.PeekContiguous(1).empty());
  std::array<std::byte, 1> output{};
  EXPECT_FALSE(cursor.ReadBytes(output));
  EXPECT_FALSE(cursor.Read<uint32_t>().has_value());
}

}  // namespace
