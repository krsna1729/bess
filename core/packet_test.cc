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

  bess::Packet::Free(seg1);
  bess::Packet::Free(seg0);
}

}  // namespace
