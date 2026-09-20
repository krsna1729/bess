// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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

#ifndef BESS_PACKET_H_
#define BESS_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include <glog/logging.h>
#include <rte_mbuf.h>

#include "metadata.h"
#include "pktbatch.h"
#include "snbuf_layout.h"

namespace bess {

// BESS's application-private packet area. DPDK places this immediately after
// struct rte_mbuf and exposes it through rte_mbuf_to_priv().
struct BessPacketPrivate {
  char metadata_[SNBUF_METADATA];
  char scratchpad_[SNBUF_SCRATCHPAD];
};

static_assert(sizeof(BessPacketPrivate) == SNBUF_METADATA + SNBUF_SCRATCHPAD,
              "BessPacketPrivate size must match BESS private data");
static_assert(sizeof(BessPacketPrivate) % RTE_MBUF_PRIV_ALIGN == 0,
              "BessPacketPrivate size must satisfy RTE_MBUF_PRIV_ALIGN");

// One centralized native pktmbuf layout shared by every PacketPool backend and
// the allocator benchmark. The data room includes DPDK headroom; BESS's
// module-visible payload limit remains SNBUF_DATA.
inline constexpr size_t kPacketPrivateSize = sizeof(BessPacketPrivate);
inline constexpr size_t kPacketDataRoomSize =
    RTE_PKTMBUF_HEADROOM + SNBUF_DATA;
inline constexpr size_t kPacketMempoolElementSize =
    sizeof(struct rte_mbuf) + sizeof(BessPacketPrivate) + kPacketDataRoomSize;

static_assert(kPacketPrivateSize <= std::numeric_limits<uint16_t>::max(),
              "Bess private data must fit in rte_mbuf::priv_size");
static_assert(kPacketDataRoomSize <= std::numeric_limits<uint16_t>::max(),
              "Packet data room must fit in rte_mbuf::buf_len");

// Non-owning view of one native packet mbuf. PacketRef is deliberately a value
// type: one pointer wide, trivially copyable, and never an owner.
class PacketRef {
 public:
  PacketRef() : pkt_(nullptr) {}
  explicit PacketRef(PacketHandle pkt) : pkt_(pkt) {}

  PacketHandle handle() const { return pkt_; }

  template <typename T = void *>
  T head_data(uint16_t offset = 0) const {
    return rte_pktmbuf_mtod_offset(pkt_, T, offset);
  }

  template <typename T = char *>
  T metadata() const {
    return reinterpret_cast<T>(rte_mbuf_to_priv(pkt_));
  }

  template <typename T = char *>
  T scratchpad() const {
    return reinterpret_cast<T>(static_cast<char *>(rte_mbuf_to_priv(pkt_)) +
                               SNBUF_METADATA);
  }

  int nb_segs() const { return pkt_->nb_segs; }
  void set_nb_segs(int n) { pkt_->nb_segs = static_cast<uint16_t>(n); }

  PacketRef next() const { return PacketRef(pkt_->next); }
  void set_next(PacketRef next) { pkt_->next = next.handle(); }

  uint16_t data_len() const { return pkt_->data_len; }
  void set_data_len(uint16_t len) { pkt_->data_len = len; }

  int head_len() const { return pkt_->data_len; }

  int total_len() const { return pkt_->pkt_len; }
  void set_total_len(uint32_t len) { pkt_->pkt_len = len; }

  uint16_t headroom() const { return rte_pktmbuf_headroom(pkt_); }
  uint16_t tailroom() const { return rte_pktmbuf_tailroom(pkt_); }

  int is_linear() const { return rte_pktmbuf_is_contiguous(pkt_); }
  int is_simple() const { return is_linear() && RTE_MBUF_DIRECT(pkt_); }

  // DPDK requires reset's input mbuf to be a single segment.
  void reset() {
    DCHECK_EQ(pkt_->nb_segs, 1);
    rte_pktmbuf_reset(pkt_);
  }

  void *prepend(uint16_t len) { return rte_pktmbuf_prepend(pkt_, len); }
  void *adj(uint16_t len) { return rte_pktmbuf_adj(pkt_, len); }
  void *append(uint16_t len) { return rte_pktmbuf_append(pkt_, len); }

  void trim(uint16_t to_remove) {
    const int ret = rte_pktmbuf_trim(pkt_, to_remove);
    DCHECK_EQ(ret, 0);
  }

  std::string Dump() const;
  void CheckSanity() const;

 private:
  PacketHandle pkt_;
};

static_assert(sizeof(PacketRef) == sizeof(void *),
              "PacketRef must stay pointer-sized");
static_assert(std::is_trivially_copyable<PacketRef>::value,
              "PacketRef must be trivially copyable");
static_assert(std::is_trivially_destructible<PacketRef>::value,
              "PacketRef must be trivially destructible");

// Ownership helpers. PacketRef is non-owning and has no destructor action.
inline void PacketFree(PacketHandle pkt) { rte_pktmbuf_free(pkt); }

inline void PacketFreeBulk(PacketHandle *pkts, size_t cnt) {
  if (cnt != 0) {
    rte_pktmbuf_free_bulk(pkts, static_cast<unsigned>(cnt));
  }
}

inline void PacketFreeBatch(PacketBatch *batch) {
  PacketFreeBulk(batch->handles(), batch->cnt());
}

// Deep-copies a linear packet's bytes using native DPDK facilities. The
// linear-packet precondition is retained from the old PacketCopy contract;
// rte_pktmbuf_copy deliberately does not copy BESS's private metadata.
PacketHandle PacketCopy(PacketHandle src);

// Defined here because both PacketRef and PacketBatch must be complete.
inline PacketRef PacketBatch::packet(size_t i) { return PacketRef(handles()[i]); }

inline PacketRef PacketBatch::packet(size_t i) const {
  return PacketRef(handles()[i]);
}

inline void PacketBatch::add(PacketRef pkt) { handles()[cnt_++] = pkt.handle(); }

}  // namespace bess

#endif  // BESS_PACKET_H_
