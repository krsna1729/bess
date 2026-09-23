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

using bess::kPacketPrivateSize;
using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::packet::EnsureContiguous;
using bess::packet::EnsureLinear;
using bess::packet::EnsureWritable;
using bess::packet::PayloadWriteability;
using bess::packet::PayloadWriteabilityOf;
using bess::packet::RemovePrefix;
using bess::packet::ReshapeError;
using bess::packet::TrimSuffix;

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

PacketHandle BuildWritableHeadSharedTail(PlainPacketPool &pool,
                                         std::span<const std::byte> head_bytes,
                                         std::span<const std::byte> tail_bytes,
                                         PacketHandle *tail_owner_out) {
  *tail_owner_out = nullptr;
  PacketHandle head = pool.AllocCopy(head_bytes.data(), head_bytes.size());
  if (head == nullptr) {
    return nullptr;
  }
  PacketHandle tail_owner =
      pool.AllocCopy(tail_bytes.data(), tail_bytes.size());
  if (tail_owner == nullptr) {
    PacketFree(head);
    return nullptr;
  }
  PacketHandle tail = bess::PacketClone(tail_owner);
  if (tail == nullptr) {
    PacketFree(tail_owner);
    PacketFree(head);
    return nullptr;
  }
  head->next = tail;
  head->nb_segs = 2;
  head->pkt_len = static_cast<uint32_t>(head_bytes.size() + tail_bytes.size());
  *tail_owner_out = tail_owner;
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
  auto *shinfo = rte_pktmbuf_ext_shinfo_init_helper(buffer, &buffer_len,
                                                    FreeExternalBuffer, owner);
  if (shinfo == nullptr) {
    delete[] buffer;
    delete owner;
    return nullptr;
  }

  PacketHandle packet =
      pool.AllocExternal(buffer, RTE_BAD_IOVA, buffer_len, shinfo, data_len);
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
  void *buf_addr;
  rte_iova_t buf_iova;
  rte_mempool *pool;
  rte_mbuf_ext_shared_info *shinfo;
  uint16_t buf_len;
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
  state.buf_addr = packet->buf_addr;
  state.buf_iova = packet->buf_iova;
  state.pool = packet->pool;
  state.shinfo = packet->shinfo;
  state.buf_len = packet->buf_len;
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
  EXPECT_EQ(actual.buf_addr, expected.buf_addr);
  EXPECT_EQ(actual.buf_iova, expected.buf_iova);
  EXPECT_EQ(actual.pool, expected.pool);
  EXPECT_EQ(actual.shinfo, expected.shinfo);
  EXPECT_EQ(actual.buf_len, expected.buf_len);
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

void ExpectPromotedHeadMetadata(PacketHandle packet,
                                const HeadState &expected) {
  EXPECT_EQ(packet->port, expected.port);
  EXPECT_EQ(packet->vlan_tci, expected.vlan_tci);
  EXPECT_EQ(packet->vlan_tci_outer, expected.vlan_tci_outer);
  EXPECT_EQ(packet->timesync, expected.timesync);
  EXPECT_EQ(packet->packet_type, expected.packet_type);
  EXPECT_EQ(packet->tx_offload, expected.tx_offload);
  EXPECT_EQ(packet->hash.rss, expected.hash_rss);
  const uint64_t representation_flags =
      RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL;
  EXPECT_EQ(packet->ol_flags & ~representation_flags,
            expected.ol_flags & ~representation_flags);
  std::array<uint32_t, 9> actual_dynfield1;
  std::copy(std::begin(packet->dynfield1), std::end(packet->dynfield1),
            actual_dynfield1.begin());
  EXPECT_EQ(actual_dynfield1, expected.dynfield1);
  EXPECT_EQ(PrivateBytes(packet), expected.private_bytes);
}

void SeedHeadMetadata(PacketHandle packet) {
  packet->port = 0x1234;
  packet->packet_type =
      RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP;
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
    if (segment->data_len != 0 && PayloadWriteabilityOf(PacketRef(segment)) !=
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

  auto null_linear = EnsureLinear(null_packet);
  ASSERT_FALSE(null_linear.has_value());
  EXPECT_EQ(null_linear.error(), ReshapeError::kNullPacket);
  auto null_contiguous = EnsureContiguous(null_packet, 0, 0);
  ASSERT_FALSE(null_contiguous.has_value());
  EXPECT_EQ(null_contiguous.error(), ReshapeError::kNullPacket);

  PlainPacketPool pool(32, -1, 256);
  const std::vector<std::byte> direct_bytes = Pattern(64);
  PacketHandle direct =
      pool.AllocCopy(direct_bytes.data(), direct_bytes.size());
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
  PlainPacketPool pool(32, -1, 256);
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
  EXPECT_EQ(clone->nb_segs, 1);
  EXPECT_EQ(clone->next, nullptr);
  EXPECT_EQ(clone->data_len, 256);
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

TEST(PacketReshapeTest,
     GenericEnsureWritableResegmentsOversizedExternalSegment) {
  PlainPacketPool pool(16, -1, 64);
  int free_count = 0;
  rte_mbuf_ext_shared_info *shinfo = nullptr;
  PacketHandle source = MakeExternal(pool, &free_count, &shinfo, 256);
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  ASSERT_TRUE(RTE_MBUF_HAS_EXTBUF(clone));
  const HeadState clone_before = TakeHeadState(clone);
  const std::vector<std::byte> source_bytes = PacketBytes(source);
  const PacketHandle old_clone = clone;
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);

  auto result = EnsureWritable(clone);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(clone, old_clone);
  EXPECT_TRUE(RTE_MBUF_DIRECT(clone));
  EXPECT_GT(clone->nb_segs, 1);
  EXPECT_EQ(clone->pkt_len, source->pkt_len);
  EXPECT_TRUE(ChainPayloadWritable(clone));
  ExpectCopiedHeadState(clone, clone_before);
  EXPECT_EQ(PacketBytes(clone), source_bytes);
  EXPECT_EQ(PacketBytes(source), source_bytes);

  FillChain(clone, static_cast<std::byte>(0xd6));
  EXPECT_EQ(PacketBytes(source), source_bytes);
  PacketFree(clone);
  PacketFree(source);
  EXPECT_EQ(free_count, 1);
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

TEST(PacketReshapeTest, EnsureLinearAlreadyLinearSharedPacketIsExactNoOp) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> bytes = Pattern(64);
  PacketHandle source = pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  ASSERT_FALSE(RTE_MBUF_DIRECT(clone));

  const PacketHandle original = clone;
  const size_t available_before = pool.Size();
  auto result = EnsureLinear(clone);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(clone, original);
  EXPECT_FALSE(RTE_MBUF_DIRECT(clone));
  EXPECT_EQ(clone->nb_segs, 1);
  EXPECT_EQ(pool.Size(), available_before);
  EXPECT_EQ(PacketBytes(clone), bytes);

  PacketFree(clone);
  PacketFree(source);
}

TEST(PacketReshapeTest, EnsureLinearUsesNativePathForWritableHead) {
  PlainPacketPool pool(8, -1, 256);
  const std::vector<std::byte> bytes = Pattern(128);
  const std::array<size_t, 2> lengths = {64, 64};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);
  const PacketHandle original = packet;
  const size_t available_before = pool.Size();

  auto result = EnsureLinear(packet);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, original);
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(packet->next, nullptr);
  EXPECT_EQ(packet->data_len, bytes.size());
  EXPECT_EQ(PacketBytes(packet), bytes);
  EXPECT_EQ(pool.Size(), available_before + 1);

  PacketFree(packet);
}

TEST(PacketReshapeTest, EnsureContiguousLinearizesWritableHeadWithSharedTail) {
  PlainPacketPool pool(8, -1, 256);
  const std::vector<std::byte> head_bytes = Pattern(64);
  const std::vector<std::byte> tail_bytes = Pattern(64);
  PacketHandle tail_owner = nullptr;
  PacketHandle packet =
      BuildWritableHeadSharedTail(pool, head_bytes, tail_bytes, &tail_owner);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(tail_owner, nullptr);
  SeedHeadMetadata(packet);
  SetPrivate(packet, static_cast<std::byte>(0x63));
  const HeadState before = TakeHeadState(packet);
  const PacketHandle original = packet;
  const PacketHandle shared_tail = packet->next;
  const size_t available_before = pool.Size();
  ASSERT_EQ(rte_mbuf_refcnt_read(tail_owner), 2);

  auto result = EnsureContiguous(packet, 32, 64);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, original);
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(packet->next, nullptr);
  EXPECT_EQ(pool.Size(), available_before + 1);
  EXPECT_EQ(rte_mbuf_refcnt_read(tail_owner), 1);
  EXPECT_EQ(PacketBytes(tail_owner), tail_bytes);
  EXPECT_EQ(packet->port, before.port);
  EXPECT_EQ(packet->packet_type, before.packet_type);
  EXPECT_EQ(packet->tx_offload, before.tx_offload);
  EXPECT_EQ(packet->hash.rss, before.hash_rss);
  EXPECT_EQ(packet->timesync, before.timesync);
  EXPECT_EQ(PrivateBytes(packet), before.private_bytes);

  std::vector<std::byte> expected = head_bytes;
  expected.insert(expected.end(), tail_bytes.begin(), tail_bytes.end());
  std::fill(expected.begin() + 32, expected.begin() + 96,
            static_cast<std::byte>(0xd3));
  EXPECT_EQ(result->data(), PacketRef(packet).head_data<std::byte *>(32));
  std::fill(result->begin(), result->end(), static_cast<std::byte>(0xd3));
  EXPECT_EQ(PacketBytes(packet), expected);
  EXPECT_NE(shared_tail, packet->next);

  PacketFree(packet);
  PacketFree(tail_owner);
}

TEST(PacketReshapeTest,
     EnsureContiguousCapacityFailurePreservesSharedChainAtomically) {
  PlainPacketPool pool(16, -1, 128);
  const std::vector<std::byte> bytes = Pattern(300);
  const std::array<size_t, 3> lengths = {128, 128, 44};
  PacketHandle source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  SetPrivate(source, static_cast<std::byte>(0x1c));
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  SetPrivate(packet, static_cast<std::byte>(0x8e));

  const HeadState source_before = TakeHeadState(source);
  const HeadState packet_before = TakeHeadState(packet);
  const PacketHandle original = packet;
  std::array<PacketHandle, 3> source_segments{};
  std::array<PacketHandle, 3> packet_segments{};
  std::array<uint16_t, 3> source_refcounts{};
  std::array<uint16_t, 3> packet_refcounts{};
  PacketHandle source_segment = source;
  PacketHandle packet_segment = packet;
  for (size_t i = 0; i < source_segments.size(); i++) {
    ASSERT_NE(source_segment, nullptr);
    ASSERT_NE(packet_segment, nullptr);
    source_segments[i] = source_segment;
    packet_segments[i] = packet_segment;
    source_refcounts[i] = rte_mbuf_refcnt_read(source_segment);
    packet_refcounts[i] = rte_mbuf_refcnt_read(packet_segment);
    source_segment = source_segment->next;
    packet_segment = packet_segment->next;
  }
  ASSERT_EQ(source_segment, nullptr);
  ASSERT_EQ(packet_segment, nullptr);
  const size_t available_before = pool.Size();

  auto result = EnsureContiguous(packet, 120, 20);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReshapeError::kInsufficientContiguousCapacity);
  EXPECT_EQ(packet, original);
  EXPECT_EQ(pool.Size(), available_before);
  ExpectHeadStateUnchanged(packet, packet_before);
  ExpectHeadStateUnchanged(source, source_before);
  for (size_t i = 0; i < source_segments.size(); i++) {
    EXPECT_EQ(source_segments[i]->next, i + 1 == source_segments.size()
                                            ? nullptr
                                            : source_segments[i + 1]);
    EXPECT_EQ(packet_segments[i]->next, i + 1 == packet_segments.size()
                                            ? nullptr
                                            : packet_segments[i + 1]);
    EXPECT_EQ(rte_mbuf_refcnt_read(source_segments[i]), source_refcounts[i]);
    EXPECT_EQ(rte_mbuf_refcnt_read(packet_segments[i]), packet_refcounts[i]);
  }

  PacketFree(packet);
  PacketFree(source);
}

TEST(PacketReshapeTest,
     EnsureContiguousAllocationFailurePreservesSharedChainAtomically) {
  PlainPacketPool pool(6, -1, 256);
  const std::vector<std::byte> bytes = Pattern(128);
  const std::array<size_t, 2> lengths = {64, 64};
  PacketHandle source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  SetPrivate(packet, static_cast<std::byte>(0x57));
  std::array<PacketHandle, 2> fillers{};
  ASSERT_TRUE(pool.AllocBulk(fillers.data(), fillers.size(), 16));
  ASSERT_EQ(pool.Size(), 0);

  const HeadState source_before = TakeHeadState(source);
  const HeadState packet_before = TakeHeadState(packet);
  const PacketHandle original = packet;
  const PacketHandle source_tail = source->next;
  const PacketHandle packet_tail = packet->next;
  const uint16_t source_head_refcount = rte_mbuf_refcnt_read(source);
  const uint16_t source_tail_refcount = rte_mbuf_refcnt_read(source_tail);
  const uint16_t packet_head_refcount = rte_mbuf_refcnt_read(packet);
  const uint16_t packet_tail_refcount = rte_mbuf_refcnt_read(packet_tail);

  auto result = EnsureContiguous(packet, 48, 32);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReshapeError::kAllocationFailed);
  EXPECT_EQ(packet, original);
  EXPECT_EQ(pool.Size(), 0);
  ExpectHeadStateUnchanged(packet, packet_before);
  ExpectHeadStateUnchanged(source, source_before);
  EXPECT_EQ(packet->next, packet_tail);
  EXPECT_EQ(packet_tail->next, nullptr);
  EXPECT_EQ(source->next, source_tail);
  EXPECT_EQ(source_tail->next, nullptr);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), source_head_refcount);
  EXPECT_EQ(rte_mbuf_refcnt_read(source_tail), source_tail_refcount);
  EXPECT_EQ(rte_mbuf_refcnt_read(packet), packet_head_refcount);
  EXPECT_EQ(rte_mbuf_refcnt_read(packet_tail), packet_tail_refcount);

  bess::PacketFreeBulk(fillers.data(), fillers.size());
  PacketFree(packet);
  PacketFree(source);
}

TEST(PacketReshapeTest, EnsureLinearDoesNotWriteSharedZeroLengthHead) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> bytes = Pattern(64);
  const std::array<size_t, 2> lengths = {0, 64};
  PacketHandle source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  ASSERT_FALSE(RTE_MBUF_DIRECT(packet));
  const PacketHandle original = packet;
  const std::vector<std::byte> source_bytes = PacketBytes(source);

  auto result = EnsureLinear(packet);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(packet, original);
  EXPECT_TRUE(RTE_MBUF_DIRECT(packet));
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(PacketBytes(packet), bytes);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), 1);
  EXPECT_EQ(PacketBytes(source), source_bytes);

  PacketFree(packet);
  PacketFree(source);
}

TEST(PacketReshapeTest, EnsureLinearReplacesWhenHeadTailroomIsInsufficient) {
  PlainPacketPool pool(16, -1, 256);
  const std::vector<std::byte> head_bytes = Pattern(128);
  const std::vector<std::byte> tail_bytes = Pattern(128);
  PacketHandle tail_owner = nullptr;
  PacketHandle packet =
      BuildWritableHeadSharedTail(pool, head_bytes, tail_bytes, &tail_owner);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(tail_owner, nullptr);

  std::memcpy(PacketRef(packet).head_data<std::byte *>(64), head_bytes.data(),
              head_bytes.size());
  packet->data_off = static_cast<uint16_t>(packet->data_off + 64);
  ASSERT_EQ(rte_pktmbuf_tailroom(packet), 64);
  const PacketHandle original = packet;

  auto result = EnsureLinear(packet);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(packet, original);
  EXPECT_TRUE(RTE_MBUF_DIRECT(packet));
  EXPECT_EQ(packet->nb_segs, 1);
  std::vector<std::byte> expected = head_bytes;
  expected.insert(expected.end(), tail_bytes.begin(), tail_bytes.end());
  EXPECT_EQ(PacketBytes(packet), expected);

  PacketFree(packet);
  PacketFree(tail_owner);
}

TEST(PacketReshapeTest, EnsureLinearRejectsPacketTooLargeForOneMbufAtomically) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> head_bytes = Pattern(128);
  const std::vector<std::byte> tail_bytes = Pattern(64);
  PacketHandle tail_owner = nullptr;
  PacketHandle packet =
      BuildWritableHeadSharedTail(pool, head_bytes, tail_bytes, &tail_owner);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(tail_owner, nullptr);
  const HeadState before = TakeHeadState(packet);
  const size_t available_before = pool.Size();

  auto result = EnsureLinear(packet);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReshapeError::kInsufficientContiguousCapacity);
  EXPECT_EQ(pool.Size(), available_before);
  ExpectHeadStateUnchanged(packet, before);

  PacketFree(packet);
  PacketFree(tail_owner);
}

TEST(PacketReshapeTest,
     EnsureLinearReplacementAllocationFailurePreservesOriginalAtomically) {
  PlainPacketPool pool(4, -1, 128);
  const std::vector<std::byte> head_bytes = Pattern(64);
  const std::vector<std::byte> tail_bytes = Pattern(64);
  PacketHandle tail_owner = nullptr;
  PacketHandle packet =
      BuildWritableHeadSharedTail(pool, head_bytes, tail_bytes, &tail_owner);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(tail_owner, nullptr);
  std::memcpy(PacketRef(packet).head_data<std::byte *>(64), head_bytes.data(),
              head_bytes.size());
  packet->data_off = static_cast<uint16_t>(packet->data_off + 64);
  ASSERT_EQ(rte_pktmbuf_tailroom(packet), 0);

  PacketHandle filler = pool.Alloc(64);
  ASSERT_NE(filler, nullptr);
  const HeadState before = TakeHeadState(packet);
  const size_t available_before = pool.Size();

  auto result = EnsureLinear(packet);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReshapeError::kAllocationFailed);
  EXPECT_EQ(pool.Size(), available_before);
  ExpectHeadStateUnchanged(packet, before);

  PacketFree(filler);
  PacketFree(packet);
  PacketFree(tail_owner);
}

TEST(PacketReshapeTest, EnsureLinearReplacementPreservesHeadMetadata) {
  PlainPacketPool pool(16, -1, 256);
  const std::vector<std::byte> bytes = Pattern(256);
  const std::array<size_t, 2> lengths = {128, 128};
  PacketHandle source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  SetPrivate(packet, static_cast<std::byte>(0xa4));
  const HeadState before = TakeHeadState(packet);
  const PacketHandle original = packet;

  auto result = EnsureLinear(packet);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(packet, original);
  EXPECT_TRUE(RTE_MBUF_DIRECT(packet));
  EXPECT_EQ(packet->nb_segs, 1);
  ExpectCopiedHeadState(packet, before);

  PacketFree(packet);
  PacketFree(source);
}

TEST(PacketReshapeTest, EnsureContiguousUsesWritableSameSegmentFastPath) {
  PlainPacketPool pool(16, -1, 128);
  const std::vector<std::byte> bytes = Pattern(256);
  const std::array<size_t, 2> lengths = {128, 128};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);
  const PacketHandle original = packet;
  const size_t available_before = pool.Size();
  const std::vector<std::byte> replacement(20, static_cast<std::byte>(0xb1));

  auto result = EnsureContiguous(packet, 8, replacement.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, original);
  EXPECT_EQ(packet->nb_segs, 2);
  EXPECT_EQ(pool.Size(), available_before);
  EXPECT_EQ(result->data(), PacketRef(packet).head_data<std::byte *>(8));
  std::copy(replacement.begin(), replacement.end(), result->begin());

  std::vector<std::byte> expected = bytes;
  std::copy(replacement.begin(), replacement.end(), expected.begin() + 8);
  EXPECT_EQ(PacketBytes(packet), expected);

  PacketFree(packet);
}

TEST(PacketReshapeTest,
     EnsureContiguousSharedLinearPacketUsesCOWWithoutChangingTopology) {
  PlainPacketPool pool(16, -1, 256);
  const std::vector<std::byte> bytes = Pattern(128);
  PacketHandle source = pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  SeedHeadMetadata(source);
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  SetPrivate(packet, static_cast<std::byte>(0x6d));
  const HeadState before = TakeHeadState(packet);
  const PacketHandle original = packet;
  const std::vector<std::byte> source_before = PacketBytes(source);
  const size_t available_before = pool.Size();
  ASSERT_EQ(packet->nb_segs, 1);
  ASSERT_EQ(packet->next, nullptr);

  auto result = EnsureContiguous(packet, 12, 24);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(packet, original);
  EXPECT_TRUE(RTE_MBUF_DIRECT(packet));
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(packet->next, nullptr);
  EXPECT_EQ(pool.Size(), available_before);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), 1);
  ExpectCopiedHeadState(packet, before);
  std::fill(result->begin(), result->end(), static_cast<std::byte>(0x92));
  EXPECT_EQ(PacketBytes(source), source_before);
  EXPECT_NE(PacketBytes(packet), source_before);

  PacketFree(packet);
  PacketFree(source);
}

TEST(PacketReshapeTest,
     EnsureContiguousOversizedSharedExternalSegmentFailsUnchanged) {
  PlainPacketPool pool(16, -1, 64);
  int free_count = 0;
  rte_mbuf_ext_shared_info *shinfo = nullptr;
  PacketHandle source = MakeExternal(pool, &free_count, &shinfo, 256);
  ASSERT_NE(source, nullptr);
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);
  const PacketHandle original = packet;
  const HeadState before = TakeHeadState(packet);
  const std::vector<std::byte> bytes = PacketBytes(packet);
  const size_t available_before = pool.Size();

  auto result = EnsureContiguous(packet, 200, 4);
  EXPECT_FALSE(result.has_value());
  if (!result.has_value()) {
    EXPECT_EQ(result.error(), ReshapeError::kInsufficientContiguousCapacity);
  }
  EXPECT_EQ(packet, original);
  EXPECT_EQ(pool.Size(), available_before);
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);
  ExpectHeadStateUnchanged(packet, before);
  EXPECT_EQ(PacketBytes(packet), bytes);
  EXPECT_EQ(PacketBytes(source), bytes);

  PacketFree(packet);
  PacketFree(source);
  EXPECT_EQ(free_count, 1);
}

TEST(PacketReshapeTest, EnsureContiguousLinearizesCrossSegmentRange) {
  PlainPacketPool pool(16, -1, 256);
  const std::vector<std::byte> bytes = Pattern(128);
  const std::array<size_t, 2> lengths = {64, 64};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);
  SeedHeadMetadata(packet);
  SetPrivate(packet, static_cast<std::byte>(0x41));
  const HeadState before = TakeHeadState(packet);
  const PacketHandle original = packet;

  auto result = EnsureContiguous(packet, 32, 64);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, original);
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(packet->next, nullptr);
  EXPECT_EQ(packet->port, before.port);
  EXPECT_EQ(packet->packet_type, before.packet_type);
  EXPECT_EQ(packet->tx_offload, before.tx_offload);
  EXPECT_EQ(packet->hash.rss, before.hash_rss);
  EXPECT_EQ(packet->timesync, before.timesync);
  EXPECT_EQ(PrivateBytes(packet), before.private_bytes);
  EXPECT_EQ(PacketBytes(packet), bytes);
  std::fill(result->begin(), result->end(), static_cast<std::byte>(0xc3));

  std::vector<std::byte> expected = bytes;
  std::fill(expected.begin() + 32, expected.begin() + 96,
            static_cast<std::byte>(0xc3));
  EXPECT_EQ(PacketBytes(packet), expected);

  PacketFree(packet);
}

TEST(PacketReshapeTest, EnsureContiguousLinearizesCrossFourSegmentRange) {
  PlainPacketPool pool(16, -1, 128);
  const std::vector<std::byte> bytes = Pattern(128);
  const std::array<size_t, 4> lengths = {32, 32, 32, 32};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);

  auto result = EnsureContiguous(packet, 16, 80);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(packet->data_len, bytes.size());
  EXPECT_EQ(PacketBytes(packet), bytes);

  PacketFree(packet);
}

TEST(PacketReshapeTest, EnsureContiguousHandlesFirstMiddleAndLastByteRanges) {
  const std::array<size_t, 3> offsets = {0, 64, 127};
  for (size_t offset : offsets) {
    SCOPED_TRACE(offset);
    PlainPacketPool pool(4, -1, 128);
    const std::vector<std::byte> bytes = Pattern(128);
    const std::array<size_t, 2> lengths = {64, 64};
    PacketHandle packet = BuildChain(pool, lengths, bytes);
    ASSERT_NE(packet, nullptr);

    auto result = EnsureContiguous(packet, offset, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1);
    EXPECT_EQ((*result)[0], bytes[offset]);
    EXPECT_EQ(packet->nb_segs, 2);

    PacketFree(packet);
  }
}

TEST(PacketReshapeTest, EnsureContiguousZeroLengthNeverChangesSharedPacket) {
  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> bytes = Pattern(64);
  PacketHandle source = pool.AllocCopy(bytes.data(), bytes.size());
  ASSERT_NE(source, nullptr);
  PacketHandle packet = bess::PacketClone(source);
  ASSERT_NE(packet, nullptr);
  const PacketHandle original = packet;
  const size_t available_before = pool.Size();

  auto first = EnsureContiguous(packet, 0, 0);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->empty());
  auto end = EnsureContiguous(packet, bytes.size(), 0);
  ASSERT_TRUE(end.has_value());
  EXPECT_TRUE(end->empty());
  EXPECT_EQ(packet, original);
  EXPECT_FALSE(RTE_MBUF_DIRECT(packet));
  EXPECT_EQ(pool.Size(), available_before);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), 2);

  auto bad_offset = EnsureContiguous(packet, bytes.size() + 1, 0);
  ASSERT_FALSE(bad_offset.has_value());
  EXPECT_EQ(bad_offset.error(), ReshapeError::kLengthOutOfRange);
  auto bad_length = EnsureContiguous(packet, 0, bytes.size() + 1);
  ASSERT_FALSE(bad_length.has_value());
  EXPECT_EQ(bad_length.error(), ReshapeError::kLengthOutOfRange);
  EXPECT_EQ(packet, original);

  PacketFree(packet);
  PacketFree(source);
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
  const std::array<Case, 7> cases = {Case::kZeroSegments,
                                     Case::kTooManySegments,
                                     Case::kExtraNext,
                                     Case::kLengthMismatch,
                                     Case::kDataOffsetPastBuffer,
                                     Case::kDataRangePastBuffer,
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
        packet->data_off =
            static_cast<uint16_t>(packet->buf_len - packet->data_len + 1);
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

    auto linear_result = EnsureLinear(packet);
    ASSERT_FALSE(linear_result.has_value());
    EXPECT_EQ(linear_result.error(), ReshapeError::kMalformedChain);
    auto contiguous_result = EnsureContiguous(packet, 0, 0);
    ASSERT_FALSE(contiguous_result.has_value());
    EXPECT_EQ(contiguous_result.error(), ReshapeError::kMalformedChain);
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

TEST(PacketReshapeTest, TopologyRemovalValidatesAndPreservesNoOps) {
  PacketHandle null_packet = nullptr;
  auto null_prefix = RemovePrefix(null_packet, 0);
  ASSERT_FALSE(null_prefix.has_value());
  EXPECT_EQ(null_prefix.error(), ReshapeError::kNullPacket);
  auto null_suffix = TrimSuffix(null_packet, 0);
  ASSERT_FALSE(null_suffix.has_value());
  EXPECT_EQ(null_suffix.error(), ReshapeError::kNullPacket);

  PlainPacketPool pool(8, -1, 128);
  const std::vector<std::byte> bytes = Pattern(64);
  const std::array<size_t, 2> lengths = {32, 32};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);
  SeedHeadMetadata(packet);
  SetPrivate(packet, static_cast<std::byte>(0x61));
  PacketHandle tail = packet->next;
  SetPrivate(tail, static_cast<std::byte>(0x2a));
  const PacketHandle original = packet;
  const HeadState head_before = TakeHeadState(packet);
  const HeadState tail_before = TakeHeadState(tail);
  const size_t available_before = pool.Size();

  auto zero_prefix = RemovePrefix(packet, 0);
  ASSERT_TRUE(zero_prefix.has_value());
  auto zero_suffix = TrimSuffix(packet, 0);
  ASSERT_TRUE(zero_suffix.has_value());
  EXPECT_EQ(packet, original);
  ExpectHeadStateUnchanged(packet, head_before);
  ExpectHeadStateUnchanged(tail, tail_before);
  EXPECT_EQ(pool.Size(), available_before);

  auto out_of_range_prefix = RemovePrefix(packet, bytes.size() + 1);
  ASSERT_FALSE(out_of_range_prefix.has_value());
  EXPECT_EQ(out_of_range_prefix.error(), ReshapeError::kLengthOutOfRange);
  auto out_of_range_suffix = TrimSuffix(packet, bytes.size() + 1);
  ASSERT_FALSE(out_of_range_suffix.has_value());
  EXPECT_EQ(out_of_range_suffix.error(), ReshapeError::kLengthOutOfRange);
  EXPECT_EQ(packet, original);
  ExpectHeadStateUnchanged(packet, head_before);
  ExpectHeadStateUnchanged(tail, tail_before);
  EXPECT_EQ(pool.Size(), available_before);

  packet->nb_segs = 1;
  const HeadState malformed_head_before = TakeHeadState(packet);
  const HeadState malformed_tail_before = TakeHeadState(tail);
  auto malformed_prefix = RemovePrefix(packet, 1);
  ASSERT_FALSE(malformed_prefix.has_value());
  EXPECT_EQ(malformed_prefix.error(), ReshapeError::kMalformedChain);
  auto malformed_suffix = TrimSuffix(packet, 1);
  ASSERT_FALSE(malformed_suffix.has_value());
  EXPECT_EQ(malformed_suffix.error(), ReshapeError::kMalformedChain);
  EXPECT_EQ(packet, original);
  ExpectHeadStateUnchanged(packet, malformed_head_before);
  ExpectHeadStateUnchanged(tail, malformed_tail_before);
  EXPECT_EQ(pool.Size(), available_before);

  packet->nb_segs = 2;
  PacketFree(packet);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketReshapeTest, PrefixRemovalHandlesBoundariesAndCrossings) {
  struct Case {
    size_t bytes_removed;
    size_t first_kept_segment;
    uint16_t first_kept_length;
  };
  constexpr std::array<Case, 6> cases = {{
      {1, 0, 31},
      {16, 0, 16},
      {32, 1, 32},
      {40, 1, 24},
      {64, 2, 32},
      {100, 3, 28},
  }};
  const std::array<size_t, 4> lengths = {32, 32, 32, 32};
  const std::vector<std::byte> bytes = Pattern(128);
  PlainPacketPool pool(32, -1, 128);

  for (const Case &test_case : cases) {
    SCOPED_TRACE(test_case.bytes_removed);
    PacketHandle packet = BuildChain(pool, lengths, bytes);
    ASSERT_NE(packet, nullptr);
    SeedHeadMetadata(packet);
    SetPrivate(packet, static_cast<std::byte>(0x73));
    const HeadState logical_head = TakeHeadState(packet);
    std::array<PacketHandle, 4> segments{};
    std::array<HeadState, 4> segment_states{};
    segments[0] = packet;
    for (size_t i = 1; i < segments.size(); i++) {
      segments[i] = segments[i - 1]->next;
    }
    for (size_t i = 0; i < segments.size(); i++) {
      segment_states[i] = TakeHeadState(segments[i]);
    }
    const size_t available_before = pool.Size();

    auto result = RemovePrefix(packet, test_case.bytes_removed);
    ASSERT_TRUE(result.has_value());
    const size_t bytes_into_head =
        test_case.bytes_removed - test_case.first_kept_segment * 32;
    EXPECT_EQ(packet, segments[test_case.first_kept_segment]);
    EXPECT_EQ(packet->data_off,
              segment_states[test_case.first_kept_segment].data_off +
                  bytes_into_head);
    EXPECT_EQ(packet->data_len, test_case.first_kept_length);
    EXPECT_EQ(packet->pkt_len, bytes.size() - test_case.bytes_removed);
    EXPECT_EQ(packet->nb_segs, segments.size() - test_case.first_kept_segment);
    EXPECT_EQ(packet->next, test_case.first_kept_segment + 1 < segments.size()
                                ? segments[test_case.first_kept_segment + 1]
                                : nullptr);
    EXPECT_EQ(packet->buf_addr,
              segment_states[test_case.first_kept_segment].buf_addr);
    EXPECT_EQ(packet->buf_iova,
              segment_states[test_case.first_kept_segment].buf_iova);
    EXPECT_EQ(packet->buf_len,
              segment_states[test_case.first_kept_segment].buf_len);
    EXPECT_EQ(packet->pool, segment_states[test_case.first_kept_segment].pool);
    EXPECT_EQ(rte_mbuf_refcnt_read(packet),
              segment_states[test_case.first_kept_segment].refcnt);
    ExpectPromotedHeadMetadata(packet, logical_head);
    EXPECT_EQ(PacketBytes(packet),
              std::vector<std::byte>(bytes.begin() + test_case.bytes_removed,
                                     bytes.end()));
    EXPECT_EQ(pool.Size(), available_before + test_case.first_kept_segment);
    for (size_t i = test_case.first_kept_segment; i < segments.size(); i++) {
      EXPECT_EQ(segments[i]->buf_addr, segment_states[i].buf_addr);
      EXPECT_EQ(segments[i]->buf_iova, segment_states[i].buf_iova);
    }

    PacketFree(packet);
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }
}

TEST(PacketReshapeTest, SuffixTrimHandlesBoundariesAndCrossings) {
  struct Case {
    size_t bytes_removed;
    size_t kept_segments;
    uint16_t last_kept_length;
  };
  constexpr std::array<Case, 6> cases = {{
      {1, 4, 31},
      {16, 4, 16},
      {32, 3, 32},
      {40, 3, 24},
      {64, 2, 32},
      {100, 1, 28},
  }};
  const std::array<size_t, 4> lengths = {32, 32, 32, 32};
  const std::vector<std::byte> bytes = Pattern(128);
  PlainPacketPool pool(32, -1, 128);

  for (const Case &test_case : cases) {
    SCOPED_TRACE(test_case.bytes_removed);
    PacketHandle packet = BuildChain(pool, lengths, bytes);
    ASSERT_NE(packet, nullptr);
    SeedHeadMetadata(packet);
    SetPrivate(packet, static_cast<std::byte>(0x85));
    const HeadState logical_head = TakeHeadState(packet);
    std::array<PacketHandle, 4> segments{};
    std::array<HeadState, 4> segment_states{};
    segments[0] = packet;
    for (size_t i = 1; i < segments.size(); i++) {
      segments[i] = segments[i - 1]->next;
    }
    for (size_t i = 0; i < segments.size(); i++) {
      segment_states[i] = TakeHeadState(segments[i]);
    }
    const size_t available_before = pool.Size();

    auto result = TrimSuffix(packet, test_case.bytes_removed);
    ASSERT_TRUE(result.has_value());
    const size_t kept_length = bytes.size() - test_case.bytes_removed;
    const size_t last_kept = test_case.kept_segments - 1;
    EXPECT_EQ(packet, segments[0]);
    EXPECT_EQ(packet->data_off, segment_states[0].data_off);
    EXPECT_EQ(packet->data_len, test_case.kept_segments == 1
                                    ? test_case.last_kept_length
                                    : segment_states[0].data_len);
    EXPECT_EQ(packet->pkt_len, kept_length);
    EXPECT_EQ(packet->nb_segs, test_case.kept_segments);
    EXPECT_EQ(segments[last_kept]->data_len, test_case.last_kept_length);
    EXPECT_EQ(segments[last_kept]->next, nullptr);
    EXPECT_EQ(packet->buf_addr, segment_states[0].buf_addr);
    EXPECT_EQ(packet->buf_iova, segment_states[0].buf_iova);
    EXPECT_EQ(packet->buf_len, segment_states[0].buf_len);
    EXPECT_EQ(packet->pool, segment_states[0].pool);
    EXPECT_EQ(rte_mbuf_refcnt_read(packet), segment_states[0].refcnt);
    ExpectPromotedHeadMetadata(packet, logical_head);
    EXPECT_EQ(
        PacketBytes(packet),
        std::vector<std::byte>(bytes.begin(), bytes.begin() + kept_length));
    EXPECT_EQ(pool.Size(),
              available_before + segments.size() - test_case.kept_segments);
    for (size_t i = 0; i < test_case.kept_segments; i++) {
      EXPECT_EQ(segments[i]->buf_addr, segment_states[i].buf_addr);
      EXPECT_EQ(segments[i]->buf_iova, segment_states[i].buf_iova);
    }

    PacketFree(packet);
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }
}

TEST(PacketReshapeTest, FullRemovalRetainsExistingZeroLengthHead) {
  const std::array<size_t, 2> lengths = {32, 32};
  const std::vector<std::byte> bytes = Pattern(64);

  for (bool remove_prefix : {true, false}) {
    SCOPED_TRACE(remove_prefix ? "prefix" : "suffix");
    PlainPacketPool pool(8, -1, 128);
    PacketHandle packet = BuildChain(pool, lengths, bytes);
    ASSERT_NE(packet, nullptr);
    SeedHeadMetadata(packet);
    SetPrivate(packet, static_cast<std::byte>(0x4a));
    const PacketHandle original = packet;
    const HeadState before = TakeHeadState(packet);
    const size_t available_before = pool.Size();

    auto result = remove_prefix ? RemovePrefix(packet, bytes.size())
                                : TrimSuffix(packet, bytes.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(packet, original);
    EXPECT_EQ(packet->next, nullptr);
    EXPECT_EQ(packet->data_off, before.data_off);
    EXPECT_EQ(packet->data_len, 0);
    EXPECT_EQ(packet->pkt_len, 0);
    EXPECT_EQ(packet->nb_segs, 1);
    EXPECT_EQ(PacketBytes(packet), std::vector<std::byte>{});
    ExpectPromotedHeadMetadata(packet, before);
    EXPECT_EQ(pool.Size(), available_before + 1);

    PacketFree(packet);
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }
}

TEST(PacketReshapeTest, SuccessfulRemovalNeedsNoAvailablePoolEntries) {
  const std::array<size_t, 4> lengths = {32, 32, 32, 32};
  const std::vector<std::byte> bytes = Pattern(128);

  {
    PlainPacketPool pool(8, -1, 128);
    PacketHandle packet = BuildChain(pool, lengths, bytes);
    ASSERT_NE(packet, nullptr);
    std::array<PacketHandle, 4> segments{};
    segments[0] = packet;
    for (size_t i = 1; i < segments.size(); i++) {
      segments[i] = segments[i - 1]->next;
    }
    std::array<PacketHandle, 4> fillers{};
    for (PacketHandle &filler : fillers) {
      filler = pool.Alloc();
      ASSERT_NE(filler, nullptr);
    }
    ASSERT_EQ(pool.Size(), 0);

    auto result = RemovePrefix(packet, 40);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(packet, segments[1]);
    EXPECT_EQ(packet->pkt_len, 88);
    EXPECT_EQ(pool.Size(), 1);
    EXPECT_EQ(PacketBytes(packet),
              std::vector<std::byte>(bytes.begin() + 40, bytes.end()));

    PacketFree(packet);
    for (PacketHandle filler : fillers) {
      PacketFree(filler);
    }
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }

  {
    PlainPacketPool pool(8, -1, 128);
    PacketHandle packet = BuildChain(pool, lengths, bytes);
    ASSERT_NE(packet, nullptr);
    std::array<PacketHandle, 4> segments{};
    segments[0] = packet;
    for (size_t i = 1; i < segments.size(); i++) {
      segments[i] = segments[i - 1]->next;
    }
    std::array<PacketHandle, 4> fillers{};
    for (PacketHandle &filler : fillers) {
      filler = pool.Alloc();
      ASSERT_NE(filler, nullptr);
    }
    ASSERT_EQ(pool.Size(), 0);

    auto result = TrimSuffix(packet, 100);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(packet, segments[0]);
    EXPECT_EQ(packet->pkt_len, 28);
    EXPECT_EQ(packet->nb_segs, 1);
    EXPECT_EQ(packet->data_len, 28);
    EXPECT_EQ(pool.Size(), 3);
    EXPECT_EQ(PacketBytes(packet),
              std::vector<std::byte>(bytes.begin(), bytes.begin() + 28));

    PacketFree(packet);
    for (PacketHandle filler : fillers) {
      PacketFree(filler);
    }
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }
}

TEST(PacketReshapeTest, HeadPromotionCopiesLogicalMetadataAndKeepsStorage) {
  PlainPacketPool pool(16, -1, 128);
  const std::vector<std::byte> bytes = Pattern(96);
  const std::array<size_t, 3> lengths = {32, 32, 32};
  PacketHandle packet = BuildChain(pool, lengths, bytes);
  ASSERT_NE(packet, nullptr);
  SeedHeadMetadata(packet);
  SetPrivate(packet, static_cast<std::byte>(0x6d));
  const HeadState logical_head = TakeHeadState(packet);

  PacketHandle promoted = packet->next;
  SeedHeadMetadata(promoted);
  promoted->ol_flags &= RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL;
  promoted->port = 0x4321;
  promoted->packet_type = 0x76543210;
  promoted->hash.rss = 0x10293847;
  promoted->vlan_tci = 0x2468;
  promoted->vlan_tci_outer = 0x1357;
  promoted->tx_offload = 0xfedcba9876543210ULL;
  promoted->timesync = 0x3344;
  for (size_t i = 0; i < std::size(promoted->dynfield1); i++) {
    promoted->dynfield1[i] = static_cast<uint32_t>(0x9000 + i);
  }
  SetPrivate(promoted, static_cast<std::byte>(0x11));
  const HeadState promoted_before = TakeHeadState(promoted);
  const PacketHandle next = promoted->next;
  const size_t available_before = pool.Size();

  auto result = RemovePrefix(packet, lengths[0]);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, promoted);
  EXPECT_EQ(packet->buf_addr, promoted_before.buf_addr);
  EXPECT_EQ(packet->buf_iova, promoted_before.buf_iova);
  EXPECT_EQ(packet->buf_len, promoted_before.buf_len);
  EXPECT_EQ(packet->data_off, promoted_before.data_off);
  EXPECT_EQ(packet->data_len, promoted_before.data_len);
  EXPECT_EQ(packet->pool, promoted_before.pool);
  EXPECT_EQ(packet->priv_size, promoted_before.priv_size);
  EXPECT_EQ(packet->shinfo, promoted_before.shinfo);
  EXPECT_EQ(rte_mbuf_refcnt_read(packet), promoted_before.refcnt);
  EXPECT_EQ(packet->next, next);
  EXPECT_EQ(packet->pkt_len, bytes.size() - lengths[0]);
  EXPECT_EQ(packet->nb_segs, 2);
  ExpectPromotedHeadMetadata(packet, logical_head);
  EXPECT_EQ(PacketBytes(packet),
            std::vector<std::byte>(bytes.begin() + lengths[0], bytes.end()));
  EXPECT_EQ(pool.Size(), available_before + 1);

  PacketFree(packet);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketReshapeTest, PromotedIndirectSegmentRetainsItsRepresentation) {
  PlainPacketPool pool(16, -1, 128);
  const std::vector<std::byte> prefix_bytes = Pattern(32);
  const std::vector<std::byte> tail_bytes = Pattern(32);
  PacketHandle owner = pool.AllocCopy(tail_bytes.data(), tail_bytes.size());
  ASSERT_NE(owner, nullptr);
  PacketHandle indirect = bess::PacketClone(owner);
  ASSERT_NE(indirect, nullptr);
  ASSERT_FALSE(RTE_MBUF_DIRECT(indirect));

  PacketHandle packet =
      pool.AllocCopy(prefix_bytes.data(), prefix_bytes.size());
  ASSERT_NE(packet, nullptr);
  packet->next = indirect;
  packet->pkt_len = prefix_bytes.size() + tail_bytes.size();
  packet->nb_segs = 2;
  SeedHeadMetadata(packet);
  SetPrivate(packet, static_cast<std::byte>(0x3e));
  const HeadState logical_head = TakeHeadState(packet);
  const HeadState promoted_before = TakeHeadState(indirect);
  const uint64_t representation_flags =
      indirect->ol_flags & (RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL);
  const size_t available_before = pool.Size();

  auto result = RemovePrefix(packet, prefix_bytes.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, indirect);
  EXPECT_FALSE(RTE_MBUF_DIRECT(packet));
  EXPECT_EQ(packet->ol_flags & (RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL),
            representation_flags);
  EXPECT_EQ(packet->buf_addr, promoted_before.buf_addr);
  EXPECT_EQ(packet->buf_iova, promoted_before.buf_iova);
  EXPECT_EQ(packet->buf_len, promoted_before.buf_len);
  EXPECT_EQ(packet->data_off, promoted_before.data_off);
  EXPECT_EQ(packet->data_len, promoted_before.data_len);
  EXPECT_EQ(packet->pool, promoted_before.pool);
  EXPECT_EQ(packet->priv_size, promoted_before.priv_size);
  EXPECT_EQ(packet->shinfo, promoted_before.shinfo);
  EXPECT_EQ(rte_mbuf_refcnt_read(packet), promoted_before.refcnt);
  EXPECT_EQ(packet->pkt_len, tail_bytes.size());
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(packet)),
            PayloadWriteability::kShared);
  ExpectPromotedHeadMetadata(packet, logical_head);
  EXPECT_EQ(PacketBytes(packet), tail_bytes);
  EXPECT_EQ(pool.Size(), available_before + 1);

  PacketFree(packet);
  PacketFree(owner);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketReshapeTest, PromotedExternalSegmentRetainsCallbackOwnership) {
  PlainPacketPool pool(16, -1, 128);
  int free_count = 0;
  rte_mbuf_ext_shared_info *shinfo = nullptr;
  PacketHandle external = MakeExternal(pool, &free_count, &shinfo, 32);
  ASSERT_NE(external, nullptr);
  const std::vector<std::byte> prefix_bytes = Pattern(32);
  PacketHandle packet =
      pool.AllocCopy(prefix_bytes.data(), prefix_bytes.size());
  ASSERT_NE(packet, nullptr);
  packet->next = external;
  packet->pkt_len = 64;
  packet->nb_segs = 2;
  SeedHeadMetadata(packet);
  SetPrivate(packet, static_cast<std::byte>(0x5b));
  const HeadState logical_head = TakeHeadState(packet);
  PacketHandle sibling = bess::PacketClone(packet);
  ASSERT_NE(sibling, nullptr);
  ASSERT_TRUE(RTE_MBUF_HAS_EXTBUF(sibling->next));
  ASSERT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);
  const HeadState promoted_before = TakeHeadState(external);
  const uint64_t representation_flags =
      external->ol_flags & (RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL);
  const std::vector<std::byte> original_bytes = PacketBytes(packet);
  const size_t available_before = pool.Size();

  auto result = RemovePrefix(packet, prefix_bytes.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(packet, external);
  EXPECT_TRUE(RTE_MBUF_HAS_EXTBUF(packet));
  EXPECT_EQ(packet->ol_flags & (RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL),
            representation_flags);
  EXPECT_EQ(packet->buf_addr, promoted_before.buf_addr);
  EXPECT_EQ(packet->buf_iova, promoted_before.buf_iova);
  EXPECT_EQ(packet->buf_len, promoted_before.buf_len);
  EXPECT_EQ(packet->data_off, promoted_before.data_off);
  EXPECT_EQ(packet->data_len, promoted_before.data_len);
  EXPECT_EQ(packet->pool, promoted_before.pool);
  EXPECT_EQ(packet->priv_size, promoted_before.priv_size);
  EXPECT_EQ(packet->shinfo, shinfo);
  EXPECT_EQ(rte_mbuf_refcnt_read(packet), promoted_before.refcnt);
  EXPECT_EQ(packet->pkt_len, 32);
  EXPECT_EQ(packet->nb_segs, 1);
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);
  EXPECT_EQ(free_count, 0);
  ExpectPromotedHeadMetadata(packet, logical_head);
  EXPECT_EQ(PacketBytes(packet), PacketBytes(external));
  EXPECT_EQ(PacketBytes(sibling), original_bytes);
  EXPECT_EQ(pool.Size(), available_before);

  PacketFree(packet);
  EXPECT_EQ(free_count, 0);
  PacketFree(sibling);
  EXPECT_EQ(free_count, 1);
  EXPECT_EQ(pool.Size(), pool.Capacity());

  free_count = 0;
  shinfo = nullptr;
  external = MakeExternal(pool, &free_count, &shinfo, 32);
  ASSERT_NE(external, nullptr);
  packet = pool.AllocCopy(prefix_bytes.data(), prefix_bytes.size());
  ASSERT_NE(packet, nullptr);
  packet->next = external;
  packet->pkt_len = 64;
  packet->nb_segs = 2;
  auto trim = TrimSuffix(packet, 32);
  ASSERT_TRUE(trim.has_value());
  EXPECT_EQ(packet->next, nullptr);
  EXPECT_EQ(packet->pkt_len, 32);
  EXPECT_EQ(free_count, 1);
  PacketFree(packet);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketReshapeTest, SharedBackingRemovalsPreserveSiblingPayloads) {
  PlainPacketPool pool(32, -1, 128);
  const std::vector<std::byte> bytes = Pattern(128);
  const std::array<size_t, 4> lengths = {32, 32, 32, 32};

  PacketHandle source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  PacketHandle sibling = bess::PacketClone(source);
  ASSERT_NE(sibling, nullptr);
  const PacketHandle promoted = source->next;
  ASSERT_EQ(rte_mbuf_refcnt_read(promoted), 2);
  const size_t prefix_available_before = pool.Size();

  auto prefix = RemovePrefix(source, 32);
  ASSERT_TRUE(prefix.has_value());
  EXPECT_EQ(source, promoted);
  EXPECT_EQ(rte_mbuf_refcnt_read(source), 2);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(source)),
            PayloadWriteability::kShared);
  EXPECT_EQ(PacketBytes(source),
            std::vector<std::byte>(bytes.begin() + 32, bytes.end()));
  EXPECT_EQ(PacketBytes(sibling), bytes);
  EXPECT_EQ(pool.Size(), prefix_available_before);

  PacketFree(source);
  PacketFree(sibling);

  source = BuildChain(pool, lengths, bytes);
  ASSERT_NE(source, nullptr);
  sibling = bess::PacketClone(source);
  ASSERT_NE(sibling, nullptr);
  const PacketHandle original_head = source;
  const PacketHandle retained_tail = source->next;
  ASSERT_EQ(rte_mbuf_refcnt_read(retained_tail), 2);
  const size_t suffix_available_before = pool.Size();

  auto suffix = TrimSuffix(source, 64);
  ASSERT_TRUE(suffix.has_value());
  EXPECT_EQ(source, original_head);
  EXPECT_EQ(source->nb_segs, 2);
  EXPECT_EQ(source->pkt_len, 64);
  EXPECT_EQ(rte_mbuf_refcnt_read(retained_tail), 2);
  EXPECT_EQ(PayloadWriteabilityOf(PacketRef(retained_tail)),
            PayloadWriteability::kShared);
  EXPECT_EQ(PacketBytes(source),
            std::vector<std::byte>(bytes.begin(), bytes.begin() + 64));
  EXPECT_EQ(PacketBytes(sibling), bytes);
  EXPECT_EQ(pool.Size(), suffix_available_before);

  PacketFree(source);
  PacketFree(sibling);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}
}  // namespace
