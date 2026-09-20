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
            bess::kPacketDataRoomSize);

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
  EXPECT_EQ(ref.tailroom(), SNBUF_DATA);
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

TEST(PacketPoolTest, AllocationRejectsOversizeAndBulkFailureIsAtomic) {
  bess::PlainPacketPool pool(4);

  EXPECT_EQ(pool.Alloc(SNBUF_DATA + 1), nullptr);

  bess::PacketHandle held[4];
  ASSERT_TRUE(pool.AllocBulk(held, 4, SNBUF_DATA));

  bess::PacketHandle extra[1] = {nullptr};
  EXPECT_FALSE(pool.AllocBulk(extra, 1, 0));
  EXPECT_EQ(extra[0], nullptr);

  bess::PacketFreeBulk(held, 4);
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
  bool *freed;
};

void FreeExternalBuffer(void *addr, void *opaque) {
  auto *owner = static_cast<ExternalBufferOwner *>(opaque);
  *owner->freed = true;
  delete[] static_cast<unsigned char *>(addr);
  delete owner;
}

TEST(PacketOwnershipTest, NativeExternalBufferFreeCallbackRuns) {
  bess::PlainPacketPool pool(4);
  bess::PacketHandle pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);

  bool freed = false;
  auto *buffer = new unsigned char[4096];
  auto *owner = new ExternalBufferOwner{&freed};
  uint16_t buffer_len = 4096;
  rte_mbuf_ext_shared_info *shinfo = rte_pktmbuf_ext_shinfo_init_helper(
      buffer, &buffer_len, FreeExternalBuffer, owner);
  ASSERT_NE(shinfo, nullptr);

  rte_pktmbuf_attach_extbuf(pkt, buffer, RTE_BAD_IOVA, buffer_len, shinfo);
  rte_pktmbuf_reset_headroom(pkt);
  bess::PacketRef ref(pkt);

  EXPECT_EQ(ref.head_data<const unsigned char *>(),
            buffer + RTE_PKTMBUF_HEADROOM);
  EXPECT_EQ(ref.headroom(), RTE_PKTMBUF_HEADROOM);
  EXPECT_EQ(ref.tailroom(), buffer_len - RTE_PKTMBUF_HEADROOM);
  ASSERT_NE(ref.append(32), nullptr);
  EXPECT_EQ(ref.data_len(), 32);

  bess::PacketFree(pkt);
  EXPECT_TRUE(freed);
}

}  // namespace
