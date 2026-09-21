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

inline constexpr size_t kPacketPrivateSize = sizeof(BessPacketPrivate);

// Every PacketPool chooses its payload capacity; the default retains the
// historical BESS limit. The DPDK data room adds headroom to that capacity.
inline constexpr size_t kDefaultPacketDataSize = SNBUF_DATA;
inline constexpr size_t kMaxPacketDataSize =
    std::numeric_limits<uint16_t>::max() - RTE_PKTMBUF_HEADROOM;
inline constexpr size_t PacketMempoolElementSize(size_t data_room_size) {
  return sizeof(struct rte_mbuf) + sizeof(BessPacketPrivate) +
         RTE_PKTMBUF_HEADROOM + data_room_size;
}

static_assert(kPacketPrivateSize <= std::numeric_limits<uint16_t>::max(),
              "Bess private data must fit in rte_mbuf::priv_size");
static_assert(RTE_PKTMBUF_HEADROOM + kDefaultPacketDataSize <=
                  std::numeric_limits<uint16_t>::max(),
              "Packet data room must fit in rte_mbuf::buf_len");
static_assert(kMaxPacketDataSize > 0,
              "DPDK headroom must fit in rte_mbuf::buf_len");

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
  // Reports room in the referenced mbuf segment. append() and trim() follow
  // DPDK's chain-tail behavior when called on a chain head.
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

namespace detail {

enum class PacketFreeBulkEligibility : uint8_t {
  kEligible,
  kZeroCount,
  kCountOverflow,
  kNullArray,
  kNullPacket,
  kMixedPool,
  kNonDirect,
  kRefcnt,
  kMultiSegment,
  kNextSegment,
};

inline PacketFreeBulkEligibility CheckPacketFreeBulkEligibility(
    PacketHandle *pkts, size_t cnt) {
  if (cnt == 0) {
    return PacketFreeBulkEligibility::kZeroCount;
  }
  if (cnt > std::numeric_limits<unsigned>::max()) {
    return PacketFreeBulkEligibility::kCountOverflow;
  }
  if (pkts == nullptr) {
    return PacketFreeBulkEligibility::kNullArray;
  }

  rte_mempool *pool = nullptr;
  for (size_t i = 0; i < cnt; i++) {
    PacketHandle pkt = pkts[i];
    if (pkt == nullptr) {
      return PacketFreeBulkEligibility::kNullPacket;
    }
    if (i == 0) {
      pool = pkt->pool;
      if (pool == nullptr) {
        return PacketFreeBulkEligibility::kNullPacket;
      }
    } else if (pkt->pool != pool) {
      return PacketFreeBulkEligibility::kMixedPool;
    }
    if (!RTE_MBUF_DIRECT(pkt)) {
      return PacketFreeBulkEligibility::kNonDirect;
    }
    if (rte_mbuf_refcnt_read(pkt) != 1) {
      return PacketFreeBulkEligibility::kRefcnt;
    }
    if (pkt->nb_segs != 1) {
      return PacketFreeBulkEligibility::kMultiSegment;
    }
    if (pkt->next != nullptr) {
      return PacketFreeBulkEligibility::kNextSegment;
    }
  }
  return PacketFreeBulkEligibility::kEligible;
}

inline bool PacketFreeBulkRawEligible(PacketHandle *pkts, size_t cnt) {
  return CheckPacketFreeBulkEligibility(pkts, cnt) ==
         PacketFreeBulkEligibility::kEligible;
}

}  // namespace detail

inline void PacketFreeBulk(PacketHandle *pkts, size_t cnt) {
  if (cnt == 0) {
    return;
  }

  DCHECK(pkts != nullptr) << "PacketFreeBulk requires a packet array";
  if (unlikely(pkts == nullptr)) {
    return;
  }

  const auto eligibility =
      detail::CheckPacketFreeBulkEligibility(pkts, cnt);
  if (eligibility == detail::PacketFreeBulkEligibility::kEligible) {
    rte_mbuf_raw_free_bulk(pkts[0]->pool, pkts,
                           static_cast<unsigned>(cnt));
  } else if (eligibility ==
             detail::PacketFreeBulkEligibility::kCountOverflow) {
    for (size_t i = 0; i < cnt; i++) {
      rte_pktmbuf_free(pkts[i]);
    }
  } else {
    rte_pktmbuf_free_bulk(pkts, static_cast<unsigned>(cnt));
  }
}

inline void PacketFreeBatch(PacketBatch *batch) {
  PacketFreeBulk(batch->handles(), batch->cnt());
}

// Creates a shallow clone. The clone has independent mbuf headers but shares
// every payload segment, including external-buffer ownership and refcounts;
// BESS's private metadata is not copied.
PacketHandle PacketClone(PacketHandle src);

// Deep-copies a packet's bytes, including all segments, using native DPDK
// facilities. BESS's private metadata is not copied.
PacketHandle PacketCopy(PacketHandle src);

// Defined here because both PacketRef and PacketBatch must be complete.
inline PacketRef PacketBatch::packet(size_t i) { return PacketRef(handles()[i]); }

inline PacketRef PacketBatch::packet(size_t i) const {
  return PacketRef(handles()[i]);
}

inline void PacketBatch::add(PacketRef pkt) { handles()[cnt_++] = pkt.handle(); }

}  // namespace bess

#endif  // BESS_PACKET_H_
