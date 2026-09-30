// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

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
// struct rte_mbuf and exposes it through rte_mbuf_to_priv(). It is logical
// packet-head state: continuation-segment private areas carry no packet-level
// semantic state and must not be treated as independent packet metadata.
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

  uint16_t head_len() const noexcept { return pkt_->data_len; }

  uint32_t total_len() const noexcept { return pkt_->pkt_len; }
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

  // Raw native DPDK escape hatches. Callers own all preconditions; checked
  // generic module-facing wrappers live in packet_mutation.h.
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

// Decision D-034 (docs/decisions.md): optimistic fast path for the common case
// (all direct, single-segment, same pool, refcnt 1).  Falls back to DPDK's
// rte_pktmbuf_free_bulk for clones, external buffers, and multi-segment chains.
inline void PacketFreeBulk(PacketHandle *pkts, size_t cnt) {
  if (unlikely(cnt == 0)) {
    return;
  }

  DCHECK(pkts != nullptr) << "PacketFreeBulk requires a packet array";
  if (unlikely(pkts == nullptr)) {
    return;
  }

  // DPDK's bulk APIs take an unsigned count. Preserve support for larger
  // inputs without narrowing the count.
  if (unlikely(cnt > std::numeric_limits<unsigned>::max())) {
    for (size_t i = 0; i < cnt; i++) {
      rte_pktmbuf_free(pkts[i]);
    }
    return;
  }

  PacketHandle first = pkts[0];
  if (unlikely(first == nullptr || first->pool == nullptr)) {
    rte_pktmbuf_free_bulk(pkts, static_cast<unsigned>(cnt));
    return;
  }
  rte_mempool *pool = first->pool;

  // Fast path: check all packets with minimal branches per packet.
  for (size_t i = 0; i < cnt; i++) {
    PacketHandle pkt = pkts[i];
    if (unlikely(pkt == nullptr || !RTE_MBUF_DIRECT(pkt) ||
                 pkt->pool != pool || rte_mbuf_refcnt_read(pkt) != 1 ||
                 pkt->nb_segs != 1 || pkt->next != nullptr)) {
      // Slow path: let DPDK handle nulls, clones, external buffers, and chains.
      rte_pktmbuf_free_bulk(pkts, static_cast<unsigned>(cnt));
      return;
    }
  }
  rte_mbuf_raw_free_bulk(pool, pkts, static_cast<unsigned>(cnt));
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
