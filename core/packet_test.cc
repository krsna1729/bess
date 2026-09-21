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

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "packet.h"
#include "packet_pool.h"
#include "utils/copy.h"

namespace {

TEST(PacketNativeLayoutTest, HandleAndPrivateAreaUseNativeMbuf) {
  static_assert(std::is_same<bess::PacketHandle, struct rte_mbuf *>::value,
                "PacketHandle must be the native rte_mbuf pointer");
  static_assert(sizeof(bess::PacketRef) == sizeof(void *),
                "PacketRef must stay pointer-sized");
  static_assert(std::is_trivially_copyable<bess::PacketRef>::value,
                "PacketRef must be trivially copyable");

  bess::PlainPacketPool pool(16);
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);

  EXPECT_EQ(rte_pktmbuf_priv_size(pool.pool()),
            static_cast<uint16_t>(sizeof(bess::BessPacketPrivate)));
  EXPECT_EQ(rte_pktmbuf_data_room_size(pool.pool()),
            RTE_PKTMBUF_HEADROOM + pool.data_room_size());

  bess::PacketRef ref(pkt);
  EXPECT_EQ(ref.metadata<uintptr_t>(),
            reinterpret_cast<uintptr_t>(rte_mbuf_to_priv(pkt)));
  EXPECT_EQ(ref.scratchpad<char *>(),
            static_cast<char *>(rte_mbuf_to_priv(pkt)) + SNBUF_METADATA);

  *ref.metadata<uint32_t *>() = 0x13579bdf;
  *ref.scratchpad<uint32_t *>() = 0xdeadbeef;
  EXPECT_EQ(*ref.metadata<uint32_t *>(), 0x13579bdfu);
  EXPECT_EQ(*ref.scratchpad<uint32_t *>(), 0xdeadbeefu);

  EXPECT_EQ(ref.headroom(), RTE_PKTMBUF_HEADROOM);
  EXPECT_EQ(ref.tailroom(), pool.data_room_size());
  EXPECT_EQ(ref.nb_segs(), 1);
  EXPECT_TRUE(ref.is_linear());
  EXPECT_TRUE(ref.is_simple());

  bess::PacketFree(pkt);
}

TEST(PacketRefTest, NativeDataOperationsUpdateMbufFields) {
  bess::PlainPacketPool pool(16);
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);

  bess::PacketRef ref(pkt);
  ASSERT_NE(ref.append(64), nullptr);
  EXPECT_EQ(ref.data_len(), 64);
  EXPECT_EQ(ref.total_len(), 64);

  ASSERT_NE(ref.prepend(16), nullptr);
  EXPECT_EQ(ref.headroom(), RTE_PKTMBUF_HEADROOM - 16);
  EXPECT_EQ(ref.data_len(), 80);
  EXPECT_EQ(ref.total_len(), 80);

  ASSERT_NE(ref.adj(8), nullptr);
  EXPECT_EQ(ref.data_len(), 72);
  EXPECT_EQ(ref.total_len(), 72);
  ref.trim(8);
  EXPECT_EQ(ref.data_len(), 64);
  EXPECT_EQ(ref.total_len(), 64);

  ref.set_data_len(3);
  ref.set_total_len(3);
  EXPECT_EQ(pkt->data_len, 3);
  EXPECT_EQ(pkt->pkt_len, 3u);

  ref.reset();
  EXPECT_EQ(ref.data_len(), 0);
  EXPECT_EQ(ref.total_len(), 0);
  EXPECT_EQ(ref.headroom(), RTE_PKTMBUF_HEADROOM);

  bess::PacketFree(pkt);
}

TEST(PacketRefTest, LargeSingleSegmentPreservesNativeRoomOperations) {
  bess::PlainPacketPool pool(4, -1, 9000);
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);

  bess::PacketRef ref(pkt);
  ASSERT_NE(ref.append(4096), nullptr);
  EXPECT_EQ(ref.tailroom(), 9000 - 4096);
  ASSERT_NE(ref.prepend(32), nullptr);
  EXPECT_EQ(ref.headroom(), RTE_PKTMBUF_HEADROOM - 32);
  EXPECT_EQ(ref.data_len(), 4096 + 32);
  EXPECT_EQ(ref.total_len(), 4096 + 32);
  ref.trim(32);
  EXPECT_EQ(ref.data_len(), 4096);
  EXPECT_EQ(ref.total_len(), 4096);
  EXPECT_EQ(ref.tailroom(), 9000 - 4096 + 32);

  bess::PacketFree(pkt);
}

TEST(PacketRefTest, MultiSegmentTraversalAndChainFree) {
  bess::PlainPacketPool pool(16);
  bess::PacketHandle first = pool.Alloc();
  bess::PacketHandle second = pool.Alloc();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  bess::PacketRef first_ref(first);
  bess::PacketRef second_ref(second);
  ASSERT_NE(first_ref.append(32), nullptr);
  ASSERT_NE(second_ref.append(16), nullptr);
  first_ref.set_next(second_ref);
  first_ref.set_nb_segs(2);
  first_ref.set_total_len(first_ref.data_len() + second_ref.data_len());

  EXPECT_EQ(first_ref.next().handle(), second);
  EXPECT_EQ(first_ref.next().next().handle(), nullptr);
  EXPECT_EQ(first_ref.nb_segs(), 2);
  EXPECT_EQ(first_ref.total_len(), 48);

  // rte_pktmbuf_free walks and returns the complete native chain.
  bess::PacketFree(first);
}

TEST(PacketPoolTest, AllocCopySupportsPcapSizedChains) {
  bess::PlainPacketPool pool(8);
  const size_t len = pool.data_room_size() + 64;
  std::vector<uint8_t> input(len);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i);
  }

  bess::PacketHandle pkt = pool.AllocCopy(input.data(), input.size());
  ASSERT_NE(pkt, nullptr);
  EXPECT_EQ(pkt->nb_segs, 2);
  ASSERT_NE(pkt->next, nullptr);
  EXPECT_EQ(pkt->data_len, pool.data_room_size());
  EXPECT_EQ(pkt->next->data_len, 64);

  std::vector<uint8_t> actual(len);
  const void *read = rte_pktmbuf_read(pkt, 0, len, actual.data());
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(std::memcmp(read, input.data(), len), 0);

  bess::PacketFree(pkt);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketPoolTest, AllocCopyFailureReclaimsPartialChain) {
  bess::PlainPacketPool pool(2, -1, 64);
  std::vector<uint8_t> input(129, 0xa5);

  EXPECT_EQ(pool.AllocCopy(input.data(), input.size()), nullptr);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketCopyTest, CopiesBytesWithoutBessPrivateMetadata) {
  bess::PlainPacketPool pool(16);
  bess::PacketHandle src = pool.Alloc();
  ASSERT_NE(src, nullptr);

  const char payload[] = "copy-me!";
  bess::PacketRef src_ref(src);
  ASSERT_NE(src_ref.append(sizeof(payload)), nullptr);
  bess::utils::Copy(src_ref.head_data(), payload, sizeof(payload));
  src_ref.metadata<uint32_t *>()[0] = 0xdeadbeef;

  bess::PacketHandle dup = bess::PacketCopy(src);
  ASSERT_NE(dup, nullptr);
  EXPECT_NE(dup, src);

  bess::PacketRef dup_ref(dup);
  EXPECT_EQ(dup_ref.total_len(), src_ref.total_len());
  EXPECT_EQ(std::memcmp(dup_ref.head_data(), src_ref.head_data(),
                        src_ref.total_len()),
            0);
  EXPECT_NE(dup_ref.metadata<uint32_t *>()[0], 0xdeadbeef);

  bess::PacketFree(dup);
  bess::PacketFree(src);
}

TEST(PacketCopyTest, CopiesChainedBytesIntoIndependentSegments) {
  bess::PlainPacketPool pool(16);
  const size_t len = pool.data_room_size() + 64;
  std::vector<uint8_t> input(len);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(0xff - i);
  }

  bess::PacketHandle src = pool.AllocCopy(input.data(), input.size());
  ASSERT_NE(src, nullptr);
  bess::PacketRef(src).metadata<uint32_t *>()[0] = 0xdeadbeef;

  bess::PacketHandle dup = bess::PacketCopy(src);
  ASSERT_NE(dup, nullptr);
  EXPECT_EQ(dup->nb_segs, 2);
  EXPECT_EQ(dup->pkt_len, input.size());
  EXPECT_NE(dup, src);
  EXPECT_NE(dup->next, nullptr);
  EXPECT_NE(dup->buf_addr, src->buf_addr);
  EXPECT_NE(dup->next->buf_addr, src->next->buf_addr);

  std::vector<uint8_t> actual(len);
  const void *read = rte_pktmbuf_read(dup, 0, len, actual.data());
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(std::memcmp(read, input.data(), len), 0);
  EXPECT_NE(bess::PacketRef(dup).metadata<uint32_t *>()[0], 0xdeadbeef);

  bess::PacketFree(dup);
  bess::PacketFree(src);
}

TEST(PacketPoolTest, AllocationRejectsOversizeAndBulkFailureIsAtomic) {
  bess::PlainPacketPool pool(4);

  EXPECT_EQ(pool.Alloc(pool.data_room_size() + 1), nullptr);

  bess::PacketHandle held[4];
  ASSERT_TRUE(pool.AllocBulk(held, 4, pool.data_room_size()));

  bess::PacketHandle extra[1] = {nullptr};
  EXPECT_FALSE(pool.AllocBulk(extra, 1, 0));
  EXPECT_EQ(extra[0], nullptr);

  bess::PacketFreeBulk(held, 4);
}

TEST(PacketPoolTest, SupportsIndependentPayloadRoomSizes) {
  bess::PlainPacketPool small_pool(4, -1, 512);
  bess::PlainPacketPool large_pool(4, -1, 4096);

  EXPECT_EQ(small_pool.data_room_size(), 512u);
  EXPECT_EQ(large_pool.data_room_size(), 4096u);
  EXPECT_EQ(rte_pktmbuf_data_room_size(small_pool.pool()),
            RTE_PKTMBUF_HEADROOM + 512);
  EXPECT_EQ(rte_pktmbuf_data_room_size(large_pool.pool()),
            RTE_PKTMBUF_HEADROOM + 4096);

  bess::PacketHandle small = small_pool.Alloc();
  bess::PacketHandle large = large_pool.Alloc();
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);
  EXPECT_EQ(bess::PacketRef(small).tailroom(), 512);
  EXPECT_EQ(bess::PacketRef(large).tailroom(), 4096);
  EXPECT_EQ(small_pool.Alloc(513), nullptr);
  EXPECT_EQ(large_pool.Alloc(4097), nullptr);

  bess::PacketFree(small);
  bess::PacketFree(large);
}

TEST(PacketPoolTest, BulkAllocationInitializesFreshNativeState) {
  bess::PlainPacketPool pool(8);

  EXPECT_TRUE(pool.AllocBulk(nullptr, 0, pool.data_room_size() + 1));
  bess::PacketFreeBulk(nullptr, 0);

  bess::PacketHandle pkts[4] = {};
  ASSERT_TRUE(pool.AllocBulk(pkts, 4, 64));
  EXPECT_TRUE(bess::detail::PacketFreeBulkRawEligible(pkts, 4));
  for (bess::PacketHandle pkt : pkts) {
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->pool, pool.pool());
    EXPECT_NE(pkt->buf_addr, nullptr);
    EXPECT_EQ(pkt->buf_len, rte_pktmbuf_data_room_size(pool.pool()));
    EXPECT_EQ(pkt->priv_size, bess::kPacketPrivateSize);
    EXPECT_NE(rte_mbuf_to_priv(pkt), nullptr);
    EXPECT_EQ(pkt->pkt_len, 64u);
    EXPECT_EQ(pkt->data_len, 64);
    EXPECT_EQ(pkt->data_off, RTE_PKTMBUF_HEADROOM);
    EXPECT_EQ(pkt->port, RTE_MBUF_PORT_INVALID);
    EXPECT_EQ(pkt->ol_flags, 0u);
    EXPECT_EQ(pkt->packet_type, 0u);
    EXPECT_EQ(pkt->tx_offload, 0u);
    EXPECT_EQ(pkt->vlan_tci, 0);
    EXPECT_EQ(pkt->vlan_tci_outer, 0);
    EXPECT_EQ(rte_mbuf_refcnt_read(pkt), 1);
    EXPECT_EQ(pkt->nb_segs, 1);
    EXPECT_EQ(pkt->next, nullptr);
    EXPECT_TRUE(RTE_MBUF_DIRECT(pkt));
    bess::PacketRef ref(pkt);
    ref.metadata<uint32_t *>()[0] = 0x13579bdf;
    EXPECT_EQ(ref.metadata<uint32_t *>()[0], 0x13579bdfu);

    pkt->pkt_len = 999;
    pkt->data_len = 999;
    pkt->data_off = 0;
    pkt->port = 7;
    pkt->ol_flags = RTE_MBUF_F_TX_IPV4;
    pkt->packet_type = 123;
    pkt->tx_offload = 0x1234;
    pkt->vlan_tci = 0x1234;
    pkt->vlan_tci_outer = 0x5678;
  }
  bess::PacketFreeBulk(pkts, 4);

  bess::PacketHandle reused[4] = {};
  ASSERT_TRUE(pool.AllocBulk(reused, 4, 32));
  EXPECT_TRUE(bess::detail::PacketFreeBulkRawEligible(reused, 4));
  for (bess::PacketHandle pkt : reused) {
    ASSERT_NE(pkt, nullptr);
    EXPECT_EQ(pkt->pkt_len, 32u);
    EXPECT_EQ(pkt->data_len, 32);
    EXPECT_EQ(pkt->data_off, RTE_PKTMBUF_HEADROOM);
    EXPECT_EQ(pkt->port, RTE_MBUF_PORT_INVALID);
    EXPECT_EQ(pkt->ol_flags, 0u);
    EXPECT_EQ(pkt->packet_type, 0u);
    EXPECT_EQ(pkt->tx_offload, 0u);
    EXPECT_EQ(pkt->vlan_tci, 0);
    EXPECT_EQ(pkt->vlan_tci_outer, 0);
    EXPECT_EQ(rte_mbuf_refcnt_read(pkt), 1);
    EXPECT_EQ(pkt->nb_segs, 1);
    EXPECT_EQ(pkt->next, nullptr);

    bess::PacketRef ref(pkt);
    ref.metadata<uint32_t *>()[0] = 0xdeadbeef;
    EXPECT_EQ(ref.metadata<uint32_t *>()[0], 0xdeadbeefu);
  }
  bess::PacketFreeBulk(reused, 4);
}

TEST(PacketOwnershipTest, BulkFreeFallsBackForMixedPools) {
  bess::PlainPacketPool first_pool(4);
  bess::PlainPacketPool second_pool(4);
  bess::PacketHandle pkts[2] = {first_pool.Alloc(), second_pool.Alloc()};
  ASSERT_NE(pkts[0], nullptr);
  ASSERT_NE(pkts[1], nullptr);

  EXPECT_FALSE(bess::detail::PacketFreeBulkRawEligible(pkts, 2));
  bess::PacketFreeBulk(pkts, 2);
  EXPECT_EQ(first_pool.Size(), 4);
  EXPECT_EQ(second_pool.Size(), 4);
}

TEST(PacketOwnershipTest, BulkFreeFallsBackForNullElement) {
  bess::PlainPacketPool pool(4);
  bess::PacketHandle pkts[3] = {pool.Alloc(), nullptr, pool.Alloc()};
  ASSERT_NE(pkts[0], nullptr);
  ASSERT_NE(pkts[2], nullptr);

  EXPECT_FALSE(bess::detail::PacketFreeBulkRawEligible(pkts, 3));
  bess::PacketFreeBulk(pkts, 3);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketOwnershipTest, BulkFreeFallsBackForMultisegmentPackets) {
  bess::PlainPacketPool pool(8);
  bess::PacketHandle first = pool.Alloc();
  bess::PacketHandle second = pool.Alloc();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  bess::PacketRef first_ref(first);
  bess::PacketRef second_ref(second);
  ASSERT_NE(first_ref.append(32), nullptr);
  ASSERT_NE(second_ref.append(16), nullptr);
  first_ref.set_next(second_ref);
  first_ref.set_nb_segs(2);
  first_ref.set_total_len(first_ref.total_len() + second_ref.total_len());

  bess::PacketHandle chain[1] = {first};
  EXPECT_FALSE(bess::detail::PacketFreeBulkRawEligible(chain, 1));
  bess::PacketFreeBulk(chain, 1);
  EXPECT_EQ(pool.Size(), 8);
}

TEST(PacketOwnershipTest, BulkFreeFallsBackForSharedReference) {
  bess::PlainPacketPool pool(4);
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);
  rte_mbuf_refcnt_update(pkt, 1);
  EXPECT_EQ(rte_mbuf_refcnt_read(pkt), 2);

  EXPECT_FALSE(bess::detail::PacketFreeBulkRawEligible(&pkt, 1));
  bess::PacketFreeBulk(&pkt, 1);
  EXPECT_EQ(rte_mbuf_refcnt_read(pkt), 1);
  bess::PacketFree(pkt);
  EXPECT_EQ(pool.Size(), 4);
}

TEST(PacketOwnershipTest, BulkFreeFallsBackForIndirectClone) {
  bess::PlainPacketPool pool(8);
  bess::PacketHandle original = pool.Alloc();
  ASSERT_NE(original, nullptr);
  ASSERT_NE(bess::PacketRef(original).append(32), nullptr);

  bess::PacketHandle clone = bess::PacketClone(original);
  ASSERT_NE(clone, nullptr);
  EXPECT_FALSE(RTE_MBUF_DIRECT(clone));
  EXPECT_FALSE(bess::detail::PacketFreeBulkRawEligible(&clone, 1));

  bess::PacketFreeBulk(&clone, 1);
  EXPECT_EQ(rte_mbuf_refcnt_read(original), 1);
  bess::PacketFree(original);
  EXPECT_EQ(pool.Size(), 8);
}

TEST(PacketBatchTest, HandlesAndRefsShareNativeIdentity) {
  bess::PlainPacketPool pool(16);
  bess::PacketBatch batch;
  batch.clear();

  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);
  batch.add(bess::PacketRef(pkt));

  EXPECT_EQ(batch.cnt(), 1);
  EXPECT_EQ(batch.handles()[0], pkt);
  EXPECT_EQ(batch.packet(0).handle(), pkt);
  const bess::PacketBatch &const_batch = batch;
  EXPECT_EQ(const_batch.packet(0).handle(), pkt);

  bess::PacketBatch copy;
  copy.Copy(&batch);
  EXPECT_EQ(copy.cnt(), 1);
  EXPECT_EQ(copy.handles()[0], pkt);

  copy.packet(0).set_data_len(5);
  EXPECT_EQ(pkt->data_len, 5);
  EXPECT_EQ(batch.packet(0).data_len(), 5);

  bess::PacketFreeBatch(&batch);
}

struct ExternalBufferOwner {
  int *free_count;
};

void FreeExternalBuffer(void *addr, void *opaque) {
  auto *owner = static_cast<ExternalBufferOwner *>(opaque);
  ++*owner->free_count;
  delete[] static_cast<unsigned char *>(addr);
  delete owner;
}

TEST(PacketOwnershipTest, NativeExternalBufferFreeCallbackRuns) {
  bess::PlainPacketPool pool(4);

  int free_count = 0;
  auto *buffer = new unsigned char[4096];
  auto *owner = new ExternalBufferOwner{&free_count};
  uint16_t buffer_len = 4096;
  rte_mbuf_ext_shared_info *shinfo = rte_pktmbuf_ext_shinfo_init_helper(
      buffer, &buffer_len, FreeExternalBuffer, owner);
  ASSERT_NE(shinfo, nullptr);

  bess::PacketHandle pkt =
      pool.AllocExternal(buffer, RTE_BAD_IOVA, buffer_len, shinfo);
  ASSERT_NE(pkt, nullptr);
  bess::PacketRef ref(pkt);

  EXPECT_EQ(ref.head_data<const unsigned char *>(),
            buffer + RTE_PKTMBUF_HEADROOM);
  EXPECT_EQ(ref.headroom(), RTE_PKTMBUF_HEADROOM);
  EXPECT_EQ(ref.tailroom(), buffer_len - RTE_PKTMBUF_HEADROOM);
  ASSERT_NE(ref.append(32), nullptr);
  EXPECT_EQ(ref.data_len(), 32);

  EXPECT_FALSE(bess::detail::PacketFreeBulkRawEligible(&pkt, 1));
  bess::PacketFreeBulk(&pkt, 1);
  EXPECT_EQ(free_count, 1);
}

TEST(PacketCloneTest, DestructionOrderingReleasesSharedStorage) {
  bess::PlainPacketPool pool(8);

  for (bool free_source_first : {true, false}) {
    bess::PacketHandle source = pool.Alloc();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(bess::PacketRef(source).append(32), nullptr);

    bess::PacketHandle clone = bess::PacketClone(source);
    ASSERT_NE(clone, nullptr);
    EXPECT_FALSE(RTE_MBUF_DIRECT(clone));
    EXPECT_EQ(rte_mbuf_refcnt_read(source), 2);
    EXPECT_EQ(bess::PacketRef(clone).head_data(), bess::PacketRef(source).head_data());

    if (free_source_first) {
      bess::PacketFree(source);
      EXPECT_EQ(pool.Size(), 6);
      bess::PacketFree(clone);
    } else {
      bess::PacketFree(clone);
      EXPECT_EQ(pool.Size(), 7);
      bess::PacketFree(source);
    }
    EXPECT_EQ(pool.Size(), pool.Capacity());
  }
}

TEST(PacketCloneTest, ClonesEverySegmentOfAChain) {
  bess::PlainPacketPool pool(8);
  const size_t len = pool.data_room_size() + 64;
  std::vector<uint8_t> input(len, 0x5a);
  bess::PacketHandle source = pool.AllocCopy(input.data(), input.size());
  ASSERT_NE(source, nullptr);

  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  ASSERT_NE(clone->next, nullptr);
  EXPECT_EQ(clone->nb_segs, source->nb_segs);
  EXPECT_EQ(clone->pkt_len, source->pkt_len);
  EXPECT_TRUE(RTE_MBUF_CLONED(clone));
  EXPECT_TRUE(RTE_MBUF_CLONED(clone->next));
  EXPECT_EQ(rte_mbuf_refcnt_read(source), 2);
  EXPECT_EQ(rte_mbuf_refcnt_read(source->next), 2);
  EXPECT_EQ(clone->buf_addr, source->buf_addr);
  EXPECT_EQ(clone->next->buf_addr, source->next->buf_addr);

  bess::PacketFree(source);
  EXPECT_EQ(pool.Size(), 4);
  bess::PacketFree(clone);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

TEST(PacketCloneTest, ExternalBufferCallbackWaitsForLastClone) {
  bess::PlainPacketPool pool(4);
  int free_count = 0;
  auto *buffer = new unsigned char[4096];
  auto *owner = new ExternalBufferOwner{&free_count};
  uint16_t buffer_len = 4096;
  rte_mbuf_ext_shared_info *shinfo = rte_pktmbuf_ext_shinfo_init_helper(
      buffer, &buffer_len, FreeExternalBuffer, owner);
  ASSERT_NE(shinfo, nullptr);

  bess::PacketHandle source =
      pool.AllocExternal(buffer, RTE_BAD_IOVA, buffer_len, shinfo, 64);
  ASSERT_NE(source, nullptr);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(clone, nullptr);
  EXPECT_TRUE(RTE_MBUF_HAS_EXTBUF(clone));
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), 2);

  bess::PacketFree(source);
  EXPECT_EQ(free_count, 0);
  bess::PacketFree(clone);
  EXPECT_EQ(free_count, 1);
  EXPECT_EQ(pool.Size(), pool.Capacity());
}

}  // namespace
