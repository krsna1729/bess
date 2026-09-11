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

#include <algorithm>
#include <cassert>
#include <string>
#include <type_traits>

#include <rte_atomic.h>
#include <rte_config.h>
#include <rte_mbuf.h>

#include "metadata.h"
#include "snbuf_layout.h"
#include "worker.h"

/* NOTE: NEVER use rte_pktmbuf_*() directly,
 *       unless you know what you are doing */
static_assert(SNBUF_MBUF == sizeof(struct rte_mbuf),
              "DPDK compatibility check failed");
static_assert(SNBUF_HEADROOM == RTE_PKTMBUF_HEADROOM,
              "DPDK compatibility check failed");

static_assert(SNBUF_IMMUTABLE_OFF == 128,
              "Packet immbutable offset must be 128");
static_assert(SNBUF_METADATA_OFF == 192, "Packet metadata offset must by 192");
static_assert(SNBUF_SCRATCHPAD_OFF == 320,
              "Packet scratchpad offset must be 320");

namespace bess {

// For the layout of snbuf, see snbuf_layout.h
class alignas(64) Packet {
 public:
  Packet() = delete;  // Packet must be allocated from PacketPool

  Packet *vaddr() const { return vaddr_; }
  void set_vaddr(Packet *addr) { vaddr_ = addr; }

  phys_addr_t paddr() { return paddr_; }
  void set_paddr(phys_addr_t addr) { paddr_ = addr; }

  uint32_t sid() const { return sid_; }
  void set_sid(uint32_t sid) { sid_ = sid; }

  uint32_t index() const { return index_; }
  void set_index(uint32_t index) { index_ = index; }

  template <typename T = char *>
  T reserve() {
    return reinterpret_cast<T>(reserve_);
  }

  template <typename T = void *>
  const T head_data(uint16_t offset = 0) const {
    return reinterpret_cast<T>(static_cast<char *>(buf_addr_) + data_off_ +
                               offset);
  }

  template <typename T = void *>
  T head_data(uint16_t offset = 0) {
    return const_cast<T>(
        static_cast<const Packet &>(*this).head_data<T>(offset));
  }

  template <typename T = char *>
  T data() {
    return reinterpret_cast<T>(data_);
  }

  template <typename T = char *>
  T metadata() const {
    return reinterpret_cast<T>(metadata_);
  }

  template <typename T = char *>
  T scratchpad() {
    return reinterpret_cast<T>(scratchpad_);
  }

  template <typename T = void *>
  T buffer() {
    return reinterpret_cast<T>(buf_addr_);
  }

  int nb_segs() const { return nb_segs_; }
  void set_nb_segs(int n) { nb_segs_ = n; }

  Packet *next() const { return next_; }
  void set_next(Packet *next) { next_ = next; }

  uint16_t data_off() { return data_off_; }
  void set_data_off(uint16_t offset) { data_off_ = offset; }

  uint16_t data_len() { return data_len_; }
  void set_data_len(uint16_t len) { data_len_ = len; }

  int head_len() const { return data_len_; }

  int total_len() const { return pkt_len_; }
  void set_total_len(uint32_t len) { pkt_len_ = len; }

  uint16_t headroom() const { return rte_pktmbuf_headroom(&mbuf_); }

  uint16_t tailroom() const { return rte_pktmbuf_tailroom(&mbuf_); }

  // single segment?
  int is_linear() const { return rte_pktmbuf_is_contiguous(&mbuf_); }

  // single segment and direct?
  int is_simple() const { return is_linear() && RTE_MBUF_DIRECT(&mbuf_); }

  void reset() { rte_pktmbuf_reset(&mbuf_); }

  void *prepend(uint16_t len) {
    if (unlikely(data_off_ < len))
      return nullptr;

    data_off_ -= len;
    data_len_ += len;
    pkt_len_ += len;

    return head_data();
  }

  // remove bytes from the beginning
  void *adj(uint16_t len) {
    if (unlikely(data_len_ < len))
      return nullptr;

    data_off_ += len;
    data_len_ -= len;
    pkt_len_ -= len;

    return head_data();
  }

  // add bytes to the end
  void *append(uint16_t len) { return rte_pktmbuf_append(&mbuf_, len); }

  // remove bytes from the end
  void trim(uint16_t to_remove) {
    int ret;

    ret = rte_pktmbuf_trim(&mbuf_, to_remove);
    DCHECK_EQ(ret, 0);
  }

  // Duplicate a new Packet object, allocated from the same PacketPool as src.
  // Returns nullptr if memory allocation failed
  static Packet *copy(const Packet *src);

  phys_addr_t dma_addr() { return buf_physaddr_ + data_off_; }

  std::string Dump();

  void CheckSanity();

  static int mt_offset_to_databuf_offset(bess::metadata::mt_offset_t offset) {
    return offset + offsetof(Packet, metadata_) - offsetof(Packet, headroom_);
  }

  // pkt may be nullptr
  static void Free(Packet *pkt) {
    rte_pktmbuf_free(reinterpret_cast<struct rte_mbuf *>(pkt));
  }

  // All pointers in pkts must not be nullptr.
  // cnt must be [0, PacketBatch::kMaxBurst]
  static inline void Free(Packet **pkts, size_t cnt);

  // batch must not be nullptr
  static void Free(PacketBatch *batch) { Free(batch->pkts(), batch->cnt()); }

 private:
  union {
    struct {
      // offset 0: Virtual address of segment buffer.
      void *buf_addr_;
      // offset 8: Physical address of segment buffer.
      alignas(8) phys_addr_t buf_physaddr_;

      union {
        __m128i rearm_data_;

        struct {
          // offset 16:
          alignas(8) uint16_t data_off_;

          // offset 18:
          uint16_t refcnt_;

          // offset 20:
          uint16_t nb_segs_;  // Number of segments

          // offset 22:
          uint16_t _dummy0_;  // rte_mbuf.port
          // offset 24:
          uint64_t _dummy1_;  // rte_mbuf.ol_flags
        };
      };

      union {
        __m128i rx_descriptor_fields1_;

        struct {
          // offset 32:
          uint32_t _dummy2_;  // rte_mbuf.packet_type_;

          // offset 36:
          uint32_t pkt_len_;  // Total pkt length: sum of all segments

          // offset 40:
          uint16_t data_len_;  // Amount of data in this segment

          // offset 42:
          uint16_t _dummy3_;  // rte_mbuf.vlan_tci

          // offset 44:
          uint32_t _dummy4_lo;  // rte_mbuf.fdir.lo and rte_mbuf.rss
        };
      };

      // offset 48:
      uint32_t _dummy4_hi;  // rte_mbuf.fdir.hi

      // offset 52:
      uint16_t _dummy5_;  // rte_mbuf.vlan_tci_outer

      // offset 54:
      const uint16_t buf_len_;

      // offset 56:
      // NOTE: as of DPDK 20.11+, rte_mbuf.pool moved from offset 72 to
      // here; the fixed "timestamp" field that used to live at this
      // offset in DPDK <20.11 no longer exists as a static struct member
      // (see the dynfield1 note below). Verified against DPDK 25.11 via
      // offsetof(); see the static_asserts after this class definition.
      struct rte_mempool *pool_;  // Pool from which mbuf was allocated.

      // 2nd cacheline - fields only used in slow path or on TX --------------
      // offset 64:
      // NOTE: aliases rte_mbuf.next (DPDK <20.11 had "userdata" here).
      // Any code that hands a multi-segment Packet to real DPDK/PMD code
      // (which walks rte_mbuf.next, not a BESS-private field) depends on
      // this offset being correct -- that's exactly what regressed
      // silently when this struct was last written against DPDK 19.11.
      Packet *next_;  // Next segment. nullptr if not scattered.

      // offset 72:
      uint64_t _dummy8;  // rte_mbuf.tx_offload

      // offset 80:
      // rte_mbuf.shinfo (external/indirect buffer support). BESS does not
      // use rte_pktmbuf_attach_extbuf(), but the field must still be
      // reserved so later offsets line up with the real struct.
      uint64_t _dummy_shinfo_;

      // offset 88:
      uint16_t _dummy9_;   // rte_mbuf.priv_size
      // offset 90:
      uint16_t _dummy10_;  // rte_mbuf.timesync

      // offset 92:
      // rte_mbuf.dynfield1: DPDK's registered-dynamic-field area. Modern
      // DPDK stores what used to be fixed fields here (timestamp,
      // userdata, seqn, ...) via rte_mbuf_dynfield_register() instead of
      // named struct members, so there is no longer a fixed field to
      // mirror by name. BESS doesn't use dynfields; this is reservation
      // only, to keep sizeof(Packet) correct.
      uint32_t _dummy_dynfield1_[9];

      // offset 128:
    };

    struct rte_mbuf mbuf_;
  };

  // Never called; exists purely so the static_asserts in its body run at
  // compile time (a member function body is a "complete-class context",
  // so offsetof(Packet, ...) is legal here even though Packet is still
  // being defined at this point in the source). Fields from `pool_`
  // onward moved between DPDK versions -- see the notes in the union
  // above -- so these checks make sure the hand-written layout there
  // always matches whatever DPDK headers this is actually compiled
  // against, instead of silently drifting the way it did between DPDK
  // 19.11 and 20.11+ (see upstream NetSys/bess#1050).
  static void CheckMbufLayout() {
    static_assert(offsetof(Packet, pool_) == offsetof(struct rte_mbuf, pool),
                  "Packet::pool_ offset must match rte_mbuf::pool");
    static_assert(offsetof(Packet, next_) == offsetof(struct rte_mbuf, next),
                  "Packet::next_ offset must match rte_mbuf::next");
    static_assert(offsetof(Packet, _dummy8) ==
                      offsetof(struct rte_mbuf, tx_offload),
                  "Packet's tx_offload placeholder offset must match "
                  "rte_mbuf::tx_offload");
    static_assert(offsetof(Packet, _dummy_shinfo_) ==
                      offsetof(struct rte_mbuf, shinfo),
                  "Packet's shinfo placeholder offset must match "
                  "rte_mbuf::shinfo");
    static_assert(offsetof(Packet, _dummy9_) ==
                      offsetof(struct rte_mbuf, priv_size),
                  "Packet's priv_size placeholder offset must match "
                  "rte_mbuf::priv_size");
    static_assert(offsetof(Packet, _dummy10_) ==
                      offsetof(struct rte_mbuf, timesync),
                  "Packet's timesync placeholder offset must match "
                  "rte_mbuf::timesync");
    static_assert(offsetof(Packet, _dummy_dynfield1_) ==
                      offsetof(struct rte_mbuf, dynfield1),
                  "Packet's dynfield1 placeholder offset must match "
                  "rte_mbuf::dynfield1");
  }

  union {
    char reserve_[SNBUF_RESERVE];

    struct {
      union {
        char immutable_[SNBUF_IMMUTABLE];

        const struct {
          // must be the first field
          Packet *vaddr_;

          phys_addr_t paddr_;

          // socket ID
          uint32_t sid_;

          // packet index within the pool
          uint32_t index_;
        };
      };

      // Dynamic metadata.
      // Each attribute value is stored in host order
      char metadata_[SNBUF_METADATA];

      // Used for module/driver-specific data
      char scratchpad_[SNBUF_SCRATCHPAD];
    };
  };

  char headroom_[SNBUF_HEADROOM];
  char data_[SNBUF_DATA];

  friend class PacketPool;
};

static_assert(std::is_standard_layout<Packet>::value, "Incorrect class Packet");
static_assert(sizeof(Packet) == SNBUF_SIZE, "Incorrect class Packet");

#if __AVX__
#include "packet_avx.h"
#else
inline void Packet::Free(Packet **pkts, size_t cnt) {
  DCHECK_LE(cnt, PacketBatch::kMaxBurst);

  // rte_mempool_put_bulk() crashes when called with cnt == 0
  if (unlikely(cnt <= 0)) {
    return;
  }

  struct rte_mempool *pool = pkts[0]->pool_;

  for (size_t i = 0; i < cnt; i++) {
    const Packet *pkt = pkts[i];

    if (unlikely(pkt->pool_ != pool || !pkt->is_simple() ||
                 pkt->refcnt_ != 1)) {
      goto slow_path;
    }
  }

  /* NOTE: it seems that zeroing the refcnt of mbufs is not necessary.
   *   (allocators will reset them) */
  rte_mempool_put_bulk(pool, reinterpret_cast<void **>(pkts), cnt);
  return;

slow_path:
  // slow path: packets are not homogeneous or simple enough
  for (size_t i = 0; i < cnt; i++) {
    Free(pkts[i]);
  }
}
#endif

}  // namespace bess

#endif  // BESS_PACKET_H_
