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

#include "packet.h"
#include "packet_pool.h"

namespace {

// Regression coverage for Phase B Stage 1 (see MODERNIZATION.md): Packet's
// pool-bookkeeping/metadata/scratchpad fields moved from named union
// members to bess::BessPacketPrivate, reached via Packet::priv() /
// rte_mbuf_to_priv() instead. This pins that the plumbing still round-trips
// and that the private struct's field ordering (metadata_ immediately
// followed by scratchpad_) matches what SNBUF_METADATA/SNBUF_SCRATCHPAD
// assume.
TEST(PacketTest, PrivateAreaRoundTrip) {
  bess::PlainPacketPool pool(16);
  bess::Packet *pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);

  // Set once by PacketPool's pool-init callback (InitPacket()).
  EXPECT_EQ(pkt->vaddr(), pkt);

  pkt->set_sid(3);
  pkt->set_index(7);
  EXPECT_EQ(pkt->sid(), 3u);
  EXPECT_EQ(pkt->index(), 7u);

  uintptr_t meta_base = pkt->metadata<uintptr_t>();
  char *scratch_base = pkt->scratchpad<char *>();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(scratch_base) - meta_base,
            static_cast<uintptr_t>(SNBUF_METADATA));

  *pkt->scratchpad<uint32_t *>() = 0xdeadbeef;
  EXPECT_EQ(*pkt->scratchpad<uint32_t *>(), 0xdeadbeefu);

  bess::Packet::Free(pkt);
}

// Cheap insurance for the multi-segment chaining fields (next_/nb_segs_),
// which live in the untouched rte_mbuf-mirroring half of Packet -- added
// while editing this file's neighboring private area, since there was no
// coverage of them anywhere in the tree.
TEST(PacketTest, MultiSegmentChaining) {
  bess::PlainPacketPool pool(16);
  bess::Packet *seg0 = pool.Alloc();
  bess::Packet *seg1 = pool.Alloc();
  ASSERT_NE(seg0, nullptr);
  ASSERT_NE(seg1, nullptr);

  EXPECT_EQ(seg0->next(), nullptr);
  EXPECT_EQ(seg0->nb_segs(), 1);

  seg0->set_next(seg1);
  seg0->set_nb_segs(2);

  EXPECT_EQ(seg0->next(), seg1);
  EXPECT_EQ(seg0->nb_segs(), 2);
  EXPECT_EQ(seg1->next(), nullptr);

  // Free(seg0) alone: rte_pktmbuf_free() walks the next_ chain and frees
  // every segment. A separate Free(seg1) here would double-free it back
  // into the mempool.
  bess::Packet::Free(seg0);
}

// Phase B Stage 2A (MODERNIZATION.md): PacketRef/PacketHandle are the
// backend-neutral seam over whatever storage backs a packet. Stage 2A keeps
// the legacy overlay underneath, so these pin that the seam resolves to
// exactly the same packet state, that a ref is a pointer-sized value with no
// ownership, and that handles stay directly usable by raw consumers.
TEST(PacketRefTest, IsPointerSizedAndTriviallyCopyable) {
  static_assert(sizeof(bess::PacketRef) == sizeof(void *),
                "PacketRef must stay pointer-sized");
  static_assert(std::is_trivially_copyable<bess::PacketRef>::value,
                "PacketRef must be trivially copyable");
  static_assert(std::is_trivially_destructible<bess::PacketRef>::value,
                "PacketRef must be trivially destructible");
  static_assert(std::is_same<bess::PacketHandle, bess::Packet *>::value,
                "Stage 2A keeps the legacy handle");

  bess::PacketRef empty;
  EXPECT_EQ(empty.handle(), nullptr);
  bess::PacketRef from_null(static_cast<bess::PacketHandle>(nullptr));
  EXPECT_EQ(from_null.handle(), nullptr);
}

TEST(PacketRefTest, ResolvesToTheSamePacketState) {
  bess::PlainPacketPool pool(16);
  bess::Packet *pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);

  bess::PacketRef ref(pkt);
  EXPECT_EQ(ref.handle(), pkt);

  EXPECT_EQ(ref.head_data(), pkt->head_data());
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ref.metadata<char *>()),
            pkt->metadata<uintptr_t>());
  EXPECT_EQ(ref.scratchpad<char *>(), pkt->scratchpad<char *>());
  EXPECT_EQ(ref.buffer(), pkt->buffer());
  EXPECT_EQ(ref.dma_addr(), pkt->dma_addr());

  void *appended = ref.append(14);
  ASSERT_NE(appended, nullptr);
  EXPECT_EQ(appended, pkt->head_data());
  EXPECT_EQ(ref.data_len(), pkt->data_len());
  EXPECT_EQ(ref.head_len(), pkt->head_len());
  EXPECT_EQ(ref.total_len(), pkt->total_len());
  EXPECT_EQ(ref.headroom(), pkt->headroom());
  EXPECT_EQ(ref.tailroom(), pkt->tailroom());
  EXPECT_EQ(ref.is_linear(), pkt->is_linear());
  EXPECT_EQ(ref.is_simple(), pkt->is_simple());

  ref.trim(4);
  EXPECT_EQ(ref.data_len(), pkt->data_len());
  EXPECT_EQ(ref.total_len(), pkt->total_len());

  // Sequenced deliberately: both sides move the same packet's data offset, so
  // comparing them in one expression would depend on evaluation order.
  void *prepended = ref.prepend(4);
  EXPECT_EQ(prepended, pkt->head_data());
  EXPECT_EQ(ref.data_off(), pkt->data_off());
  void *adjusted = ref.adj(2);
  EXPECT_EQ(adjusted, pkt->head_data());
  EXPECT_EQ(ref.data_off(), pkt->data_off());

  ref.set_data_len(3);
  EXPECT_EQ(pkt->data_len(), 3);
  ref.set_total_len(3);
  EXPECT_EQ(pkt->total_len(), 3);

  bess::PacketFree(ref.handle());
}

TEST(PacketRefTest, MultiSegmentTraversalRoundTrips) {
  bess::PlainPacketPool pool(16);
  bess::Packet *first = pool.Alloc();
  bess::Packet *second = pool.Alloc();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  first->set_next(second);
  first->set_nb_segs(2);

  bess::PacketRef ref(first);
  EXPECT_EQ(ref.nb_segs(), 2);
  EXPECT_EQ(ref.next().handle(), second);
  EXPECT_EQ(ref.next().next().handle(), nullptr);

  ref.next().set_data_len(7);
  EXPECT_EQ(second->data_len(), 7);

  ref.set_nb_segs(2);
  EXPECT_EQ(first->nb_segs(), 2);

  // Freeing the head frees the whole chain (rte_pktmbuf_free semantics).
  bess::PacketFree(ref.handle());
}

TEST(PacketRefTest, CopyIsADeepCopyOfBytesAndNotMetadata) {
  bess::PlainPacketPool pool(16);
  bess::Packet *src = pool.Alloc();
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
  EXPECT_EQ(memcmp(dup_ref.head_data(), src_ref.head_data(),
                   src_ref.total_len()), 0);
  // BESS metadata is not part of the copy.
  EXPECT_NE(dup_ref.metadata<uint32_t *>()[0], 0xdeadbeef);

  bess::PacketFree(dup);
  bess::PacketFree(src);
}

TEST(PacketBatchSeamTest, HandlesAndRefsReferToTheSamePacket) {
  bess::PlainPacketPool pool(16);
  bess::PacketBatch batch;
  batch.clear();  // PacketBatch stays a POD; cnt_ is not initialized by a ctor

  bess::Packet *pkt = pool.Alloc();
  ASSERT_NE(pkt, nullptr);
  batch.add(bess::PacketRef(pkt));

  EXPECT_EQ(batch.cnt(), 1);
  EXPECT_EQ(batch.handles()[0], pkt);
  EXPECT_EQ(batch.packet(0).handle(), pkt);
  const bess::PacketBatch &const_batch = batch;
  EXPECT_EQ(const_batch.packet(0).handle(), pkt);

  // Batch copies stay pointer-array copies: writing through the copy reaches
  // the same packet. (Neither batch owns anything, so the packet must be
  // freed exactly once, by whichever batch is designated the owner.)
  bess::PacketBatch copy;
  copy.Copy(&batch);
  EXPECT_EQ(copy.cnt(), 1);
  EXPECT_EQ(copy.handles()[0], pkt);
  EXPECT_EQ(copy.packet(0).handle(), pkt);

  copy.packet(0).set_data_len(5);
  EXPECT_EQ(pkt->data_len(), 5);
  EXPECT_EQ(batch.packet(0).data_len(), 5);

  bess::PacketFreeBatch(&batch);
}

}  // namespace
