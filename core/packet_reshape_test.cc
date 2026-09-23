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
#include <vector>

#include "packet_mutation.h"
#include "packet_pool.h"
#include "packet_reshape.h"

namespace {

using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::kPacketPrivateSize;
using bess::packet::EnsureWritable;
using bess::packet::PayloadWriteability;
using bess::packet::PayloadWriteabilityOf;
using bess::packet::ReshapeError;

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

std::vector<std::byte> PacketBytes(PacketHandle packet) {
  std::vector<std::byte> bytes(packet->pkt_len);
  if (!bytes.empty()) {
    const void *read = rte_pktmbuf_read(packet, 0, bytes.size(), bytes.data());
    if (read == nullptr) {
      return bytes;
    }
    if (read != bytes.data()) {
      std::memcpy(bytes.data(), read, bytes.size());
    }
  }
  return bytes;
}

std::vector<std::byte> PrivateBytes(PacketHandle packet) {
  std::vector<std::byte> bytes(kPacketPrivateSize);
  std::memcpy(bytes.data(), rte_mbuf_to_priv(packet), bytes.size());
  return bytes;
}

void SetPrivate(PacketHandle packet, std::byte value) {
  std::fill_n(static_cast<std::byte *>(rte_mbuf_to_priv(packet)),
              kPacketPrivateSize, value);
}

struct HeadState {
  PacketHandle handle;
  PacketHandle next;
  uint16_t data_off;
  uint16_t data_len;
  uint16_t nb_segs;
  uint16_t port;
  uint16_t vlan_tci;
  uint16_t vlan_tci_outer;
  uint16_t priv_size;
  uint16_t timesync;
  uint32_t pkt_len;
  uint32_t packet_type;
  uint64_t ol_flags;
  uint64_t tx_offload;
  uint32_t hash_rss;
  uint16_t refcnt;
  std::array<uint32_t, 9> dynfield1;
  std::vector<std::byte> private_bytes;
  std::vector<std::byte> payload;
};

HeadState TakeHeadState(PacketHandle packet) {
  HeadState state;
  state.handle = packet;
  state.next = packet->next;
  state.data_off = packet->data_off;
  state.data_len = packet->data_len;
  state.nb_segs = packet->nb_segs;
  state.port = packet->port;
  state.vlan_tci = packet->vlan_tci;
  state.vlan_tci_outer = packet->vlan_tci_outer;
  state.priv_size = packet->priv_size;
  state.timesync = packet->timesync;
  state.pkt_len = packet->pkt_len;
  state.packet_type = packet->packet_type;
  state.ol_flags = packet->ol_flags;
  state.tx_offload = packet->tx_offload;
  state.hash_rss = packet->hash.rss;
  state.refcnt = rte_mbuf_refcnt_read(packet);
  std::copy(std::begin(packet->dynfield1), std::end(packet->dynfield1),
            state.dynfield1.begin());
  state.private_bytes = PrivateBytes(packet);
  state.payload = PacketBytes(packet);
  return state;
}

void ExpectHeadStateUnchanged(PacketHandle packet, const HeadState &expected) {
  const HeadState actual = TakeHeadState(packet);
  EXPECT_EQ(actual.handle, expected.handle);
  EXPECT_EQ(actual.next, expected.next);
  EXPECT_EQ(actual.data_off, expected.data_off);
  EXPECT_EQ(actual.data_len, expected.data_len);
  EXPECT_EQ(actual.nb_segs, expected.nb_segs);
  EXPECT_EQ(actual.port, expected.port);
  EXPECT_EQ(actual.vlan_tci, expected.vlan_tci);
  EXPECT_EQ(actual.vlan_tci_outer, expected.vlan_tci_outer);
  EXPECT_EQ(actual.priv_size, expected.priv_size);
  EXPECT_EQ(actual.timesync, expected.timesync);
  EXPECT_EQ(actual.pkt_len, expected.pkt_len);
  EXPECT_EQ(actual.packet_type, expected.packet_type);
  EXPECT_EQ(actual.ol_flags, expected.ol_flags);
  EXPECT_EQ(actual.tx_offload, expected.tx_offload);
  EXPECT_EQ(actual.hash_rss, expected.hash_rss);
  EXPECT_EQ(actual.refcnt, expected.refcnt);
  EXPECT_EQ(actual.dynfield1, expected.dynfield1);
  EXPECT_EQ(actual.private_bytes, expected.private_bytes);
  EXPECT_EQ(actual.payload, expected.payload);
}

void ExpectCopiedHeadState(PacketHandle packet, const HeadState &expected) {
  EXPECT_EQ(packet->port, expected.port);
  EXPECT_EQ(packet->vlan_tci, expected.vlan_tci);
  EXPECT_EQ(packet->vlan_tci_outer, expected.vlan_tci_outer);
  EXPECT_EQ(packet->priv_size, expected.priv_size);
  EXPECT_EQ(packet->timesync, expected.timesync);
  EXPECT_EQ(packet->pkt_len, expected.pkt_len);
  EXPECT_EQ(packet->packet_type, expected.packet_type);
  EXPECT_EQ(packet->tx_offload, expected.tx_offload);
  EXPECT_EQ(packet->hash.rss, expected.hash_rss);
  std::array<uint32_t, 9> actual_dynfield1;
  std::copy(std::begin(packet->dynfield1), std::end(packet->dynfield1),
            actual_dynfield1.begin());
  EXPECT_EQ(actual_dynfield1, expected.dynfield1);
  EXPECT_EQ(PrivateBytes(packet), expected.private_bytes);
  EXPECT_EQ(PacketBytes(packet), expected.payload);

  const uint64_t representation_flags =
      RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL;
  EXPECT_EQ(packet->ol_flags, expected.ol_flags & ~representation_flags);
}

void SeedHeadMetadata(PacketHandle packet) {
  packet->port = 0x1234;
  packet->packet_type = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 |
                        RTE_PTYPE_L4_UDP;
  packet->hash.rss = 0x89abcdef;
  packet->vlan_tci = 0x1357;
  packet->vlan_tci_outer = 0x2468;
  packet->tx_offload = 0x0123456789abcdefULL;
  packet->ol_flags =
      (packet->ol_flags & (RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL)) |
      RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_QINQ | RTE_MBUF_F_RX_RSS_HASH |
      RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
  packet->timesync = 0x55aa;
  for (size_t i = 0; i < std::size(packet->dynfield1); i++) {
    packet->dynfield1[i] = static_cast<uint32_t>(0x1000 + i * 17);
  }
}

bool ChainPayloadWritable(PacketHandle packet) {
  for (PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    if (segment->data_len != 0 &&
        PayloadWriteabilityOf(PacketRef(segment)) !=
            PayloadWriteability::kWritable) {
      return false;
    }
  }
  return true;
}

void FillChain(PacketHandle packet, std::byte value) {
  for (PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    std::fill_n(PacketRef(segment).head_data<std::byte *>(), segment->data_len,
                value);
  }
}

TEST(PacketReshapeTest, NullAndUniquePacketsAreStrictNoOps) {
  PacketHandle null_packet = nullptr;
  auto null_result = EnsureWritable(null_packet);
  ASSERT_FALSE(null_result.has_value());
  EXPECT_EQ(null_result.error(), ReshapeError::kNullPacket);
  EXPECT_EQ(null_packet, nullptr);

  PlainPacketPool pool(32, -1, 256);
  const std::vector<std::byte> direct_bytes = Pattern(64);
  PacketHandle direct = pool.AllocCopy(direct_bytes.data(), direct_bytes.size());
  ASSERT_NE(direct, nullptr);
  const PacketHandle direct_head = direct;
  const size_t direct_available = pool.Size();
  auto direct_result = EnsureWritable(direct);
  ASSERT_TRUE(direct_result.has_value());
  EXPECT_EQ(direct, direct_head);
  EXPECT_EQ(pool.Size(), direct_available);

  const std::vector<std::byte> chain_bytes = Pattern(512);
  const std::array<size_t, 2> lengths = {256, 256};
  PacketHandle chain = BuildChain(pool, lengths, chain_bytes);
  ASSERT_NE(chain, nullptr);
  const PacketHandle chain_head = chain;
  const PacketHandle chain_tail = chain->next;
  const size_t chain_available = pool.Size();
  auto chain_result = EnsureWritable(chain);
  ASSERT_TRUE(chain_result.has_value());
  EXPECT_EQ(chain, chain_head);
  EXPECT_EQ(chain->next, chain_tail);
  EXPECT_EQ(chain->nb_segs, 2);
  EXPECT_EQ(pool.Size(), chain_available);

  PacketFree(chain);
  PacketFree(direct);
}

TEST(PacketReshapeTest, SharedDirectAndIndirectPacketsUseWholePacketCOW) {
  PlainPacketPool pool(32, -1, 128);
  const std::vector<std::byte> bytes = Pattern(256);
  const std::array<size_t, 2> lengths = {128, 128};
  PacketHandle source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);

  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  ASSERT_FALSE(RTE_MBUF_DIRECT(clone));
  SetPrivate(clone, static_cast<std::byte>(0x4c));
  const HeadState clone_before = TakeHeadState(clone);
  const std::vector<std::byte> source_bytes = PacketBytes(source);
  const PacketHandle old_clone = clone;
  const uint16_t source_refcnt_before = rte_mbuf_refcnt_read(source);

  auto result = EnsureWritable(clone);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(clone, old_clone);
  EXPECT_TRUE(RTE_MBUF_DIRECT(clone));
  EXPECT_TRUE(ChainPayloadWritable(clone));
  ExpectCopiedHeadState(clone, clone_before);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), 1);
  EXPECT_EQ(source_refcnt_before, 2);
  EXPECT_EQ(PacketBytes(source), source_bytes);

  FillChain(clone, static_cast<std::byte>(0xa7));
  EXPECT_NE(PacketBytes(clone), source_bytes);
  EXPECT_EQ(PacketBytes(source), source_bytes);

  PacketFree(clone);
  PacketFree(source);
}

TEST(PacketReshapeTest, SharedExternalPacketUsesWholePacketCOW) {
  PlainPacketPool pool(16, -1, 128);
  int free_count = 0;
  rte_mbuf_ext_shared_info *shinfo = nullptr;
  PacketHandle source = MakeExternal(pool, &free_count, &shinfo, 64);
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  ASSERT_TRUE(RTE_MBUF_HAS_EXTBUF(clone));
  SetPrivate(clone, static_cast<std::byte>(0x59));
  const HeadState clone_before = TakeHeadState(clone);
  const std::vector<std::byte> source_bytes = PacketBytes(source);
  const PacketHandle old_clone = clone;
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);

  auto result = EnsureWritable(clone);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(clone, old_clone);
  EXPECT_TRUE(RTE_MBUF_DIRECT(clone));
  EXPECT_TRUE(ChainPayloadWritable(clone));
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 1);
  ExpectCopiedHeadState(clone, clone_before);
  EXPECT_EQ(PacketBytes(source), source_bytes);

  FillChain(clone, static_cast<std::byte>(0xb8));
  EXPECT_EQ(PacketBytes(source), source_bytes);

  PacketFree(clone);
  PacketFree(source);
  EXPECT_EQ(free_count, 1);
}

TEST(PacketReshapeTest, UniqueAgainAfterBackingReleaseStaysOnFastPath) {
  PlainPacketPool pool(16, -1, 128);
  PacketHandle source = pool.Alloc(64);
  ASSERT_NE(source, nullptr);
  PacketHandle indirect = bess::PacketClone(source);
  ASSERT_NE(indirect, nullptr);
  PacketFree(source);
  const PacketHandle indirect_head = indirect;
  const size_t indirect_available = pool.Size();

  auto indirect_result = EnsureWritable(indirect);
  ASSERT_TRUE(indirect_result.has_value());
  EXPECT_EQ(indirect, indirect_head);
  EXPECT_EQ(pool.Size(), indirect_available);
  PacketFree(indirect);

  int free_count = 0;
  rte_mbuf_ext_shared_info *shinfo = nullptr;
  PacketHandle external = MakeExternal(pool, &free_count, &shinfo, 64);
  ASSERT_NE(external, nullptr);
  PacketHandle external_clone = bess::PacketClone(external);
  ASSERT_NE(external_clone, nullptr);
  PacketFree(external_clone);
  ASSERT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 1);
  const PacketHandle external_head = external;
  const size_t external_available = pool.Size();

  auto external_result = EnsureWritable(external);
  ASSERT_TRUE(external_result.has_value());
  EXPECT_EQ(external, external_head);
  EXPECT_EQ(pool.Size(), external_available);
  PacketFree(external);
  EXPECT_EQ(free_count, 1);
}

TEST(PacketReshapeTest, COWPreservesBessAndDPDKHeadState) {
  PlainPacketPool pool(16, -1, 256);
  const std::vector<std::byte> bytes = Pattern(128);
  PacketHandle source = pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  SetPrivate(source, static_cast<std::byte>(0x31));
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  SetPrivate(clone, static_cast<std::byte>(0xd2));
  const HeadState clone_before = TakeHeadState(clone);

  auto result = EnsureWritable(clone);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(RTE_MBUF_DIRECT(clone));
  ExpectCopiedHeadState(clone, clone_before);
  EXPECT_EQ(PrivateBytes(clone), clone_before.private_bytes);
  EXPECT_EQ(PacketBytes(clone), bytes);

  PacketFree(clone);
  PacketFree(source);
}

TEST(PacketReshapeTest, AllocationFailurePreservesOriginalPacketAtomically) {
  PlainPacketPool pool(4, -1, 128);
  const std::vector<std::byte> bytes = Pattern(64);
  PacketHandle source = pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);

  PacketHandle fillers[2] = {nullptr, nullptr};
  ASSERT_TRUE(pool.AllocBulk(fillers, 2, 64));
  const HeadState clone_before = TakeHeadState(clone);
  const HeadState source_before = TakeHeadState(source);
  const PacketHandle original = clone;

  auto result = EnsureWritable(clone);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReshapeError::kAllocationFailed);
  EXPECT_EQ(clone, original);
  ExpectHeadStateUnchanged(clone, clone_before);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), source_before.refcnt);
  EXPECT_EQ(PacketBytes(source), source_before.payload);

  bess::PacketFreeBulk(fillers, 2);
  PacketFree(clone);
  PacketFree(source);
}

TEST(PacketReshapeTest,
     PartialReplacementAllocationFailurePreservesOriginalAtomically) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> bytes = Pattern(300);
  PacketHandle source = pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  ASSERT_EQ(source->nb_segs, 3);
  SetPrivate(source, static_cast<std::byte>(0x2a));

  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  ASSERT_EQ(clone->nb_segs, 3);
  SetPrivate(clone, static_cast<std::byte>(0x7b));
  ASSERT_EQ(pool.Size(), 2);

  const HeadState clone_before = TakeHeadState(clone);
  const HeadState source_before = TakeHeadState(source);
  const PacketHandle original = clone;
  std::array<PacketHandle, 3> clone_segments{};
  std::array<uint16_t, 3> clone_data_lengths{};
  std::array<uint16_t, 3> clone_refcounts{};
  PacketHandle segment = clone;
  for (size_t i = 0; i < clone_segments.size(); i++) {
    ASSERT_NE(segment, nullptr);
    clone_segments[i] = segment;
    clone_data_lengths[i] = segment->data_len;
    clone_refcounts[i] = rte_mbuf_refcnt_read(segment);
    segment = segment->next;
  }
  ASSERT_EQ(segment, nullptr);

  const size_t available_before = pool.Size();
  auto result = EnsureWritable(clone);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReshapeError::kAllocationFailed);
  EXPECT_EQ(clone, original);
  EXPECT_EQ(pool.Size(), available_before);
  ExpectHeadStateUnchanged(clone, clone_before);
  ExpectHeadStateUnchanged(source, source_before);

  segment = clone;
  for (size_t i = 0; i < clone_segments.size(); i++) {
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(segment, clone_segments[i]);
    EXPECT_EQ(segment->data_len, clone_data_lengths[i]);
    EXPECT_EQ(rte_mbuf_refcnt_read(segment), clone_refcounts[i]);
    segment = segment->next;
  }
  EXPECT_EQ(segment, nullptr);

  PacketFree(clone);
  PacketFree(source);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketReshapeTest, MalformedChainFailsWithoutMutation) {
  enum class Case : uint8_t {
    kZeroSegments,
    kTooManySegments,
    kExtraNext,
    kLengthMismatch,
    kDataOffsetPastBuffer,
    kDataRangePastBuffer,
    kMissingPool,
  };
  const std::array<const char *, 7> names = {
      "nb_segs == 0",
      "nb_segs exceeds actual chain",
      "extra next beyond nb_segs",
      "sum(data_len) != pkt_len",
      "data_off > buf_len",
      "data_off + data_len > buf_len",
      "pool == nullptr",
  };
  const std::array<Case, 7> cases = {
      Case::kZeroSegments,       Case::kTooManySegments,
      Case::kExtraNext,          Case::kLengthMismatch,
      Case::kDataOffsetPastBuffer, Case::kDataRangePastBuffer,
      Case::kMissingPool};

  for (size_t case_index = 0; case_index < cases.size(); case_index++) {
    SCOPED_TRACE(names[case_index]);
    PlainPacketPool pool(8, -1, 64);
    const std::vector<std::byte> bytes = Pattern(16);
    PacketHandle packet = pool.AllocCopy(bytes.data(), bytes.size());
    ASSERT_NE(packet, nullptr);

    const HeadState before = TakeHeadState(packet);
    const PacketHandle original = packet;
    rte_mempool *initial_pool = packet->pool;
    const uint16_t initial_data_off = packet->data_off;
    const uint16_t initial_data_len = packet->data_len;
    const uint16_t initial_nb_segs = packet->nb_segs;
    const uint32_t initial_pkt_len = packet->pkt_len;
    const uint16_t initial_buf_len = packet->buf_len;
    PacketHandle extra = nullptr;

    switch (cases[case_index]) {
      case Case::kZeroSegments:
        packet->nb_segs = 0;
        break;
      case Case::kTooManySegments:
        packet->nb_segs = 2;
        break;
      case Case::kExtraNext:
        extra = pool.Alloc();
        ASSERT_NE(extra, nullptr);
        packet->next = extra;
        packet->nb_segs = 1;
        break;
      case Case::kLengthMismatch:
        packet->pkt_len = initial_pkt_len + 1;
        break;
      case Case::kDataOffsetPastBuffer:
        ASSERT_LT(packet->buf_len, std::numeric_limits<uint16_t>::max());
        packet->data_off = static_cast<uint16_t>(packet->buf_len + 1);
        break;
      case Case::kDataRangePastBuffer:
        ASSERT_GT(packet->buf_len, packet->data_len);
        packet->data_off = static_cast<uint16_t>(
            packet->buf_len - packet->data_len + 1);
        break;
      case Case::kMissingPool:
        packet->pool = nullptr;
        break;
    }

    const PacketHandle expected_next = packet->next;
    rte_mempool *expected_pool = packet->pool;
    const uint16_t expected_data_off = packet->data_off;
    const uint16_t expected_data_len = packet->data_len;
    const uint16_t expected_nb_segs = packet->nb_segs;
    const uint32_t expected_pkt_len = packet->pkt_len;
    const size_t available_before = pool.Size();

    auto result = EnsureWritable(packet);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ReshapeError::kMalformedChain);
    EXPECT_EQ(packet, original);
    EXPECT_EQ(pool.Size(), available_before);
    EXPECT_EQ(packet->next, expected_next);
    EXPECT_EQ(packet->pool, expected_pool);
    EXPECT_EQ(packet->data_off, expected_data_off);
    EXPECT_EQ(packet->data_len, expected_data_len);
    EXPECT_EQ(packet->nb_segs, expected_nb_segs);
    EXPECT_EQ(packet->pkt_len, expected_pkt_len);
    EXPECT_EQ(packet->buf_len, initial_buf_len);
    EXPECT_EQ(packet->priv_size, before.priv_size);
    EXPECT_EQ(packet->timesync, before.timesync);
    EXPECT_EQ(packet->port, before.port);
    EXPECT_EQ(packet->vlan_tci, before.vlan_tci);
    EXPECT_EQ(packet->vlan_tci_outer, before.vlan_tci_outer);
    EXPECT_EQ(packet->packet_type, before.packet_type);
    EXPECT_EQ(packet->ol_flags, before.ol_flags);
    EXPECT_EQ(packet->tx_offload, before.tx_offload);
    EXPECT_EQ(packet->hash.rss, before.hash_rss);
    EXPECT_EQ(rte_mbuf_refcnt_read(packet), before.refcnt);
    std::array<uint32_t, 9> actual_dynfield1;
    std::copy(std::begin(packet->dynfield1), std::end(packet->dynfield1),
              actual_dynfield1.begin());
    EXPECT_EQ(actual_dynfield1, before.dynfield1);
    EXPECT_EQ(PrivateBytes(packet), before.private_bytes);

    const bool invalid_payload_bounds =
        cases[case_index] == Case::kDataOffsetPastBuffer ||
        cases[case_index] == Case::kDataRangePastBuffer;
    if (!invalid_payload_bounds) {
      std::vector<std::byte> actual_payload(packet->data_len);
      std::memcpy(actual_payload.data(), PacketRef(packet).head_data(),
                  actual_payload.size());
      EXPECT_EQ(actual_payload, before.payload);
    }

    packet->next = nullptr;
    packet->data_off = initial_data_off;
    packet->data_len = initial_data_len;
    packet->nb_segs = initial_nb_segs;
    packet->pkt_len = initial_pkt_len;
    packet->pool = initial_pool;
    if (extra != nullptr) {
      PacketFree(extra);
    }
    PacketFree(packet);
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }
}

}  // namespace
