// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "packet.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "utils/copy.h"
#include "utils/format.h"

namespace bess {
namespace {

void HexDump(std::ostringstream *dump, const void *data, size_t len) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; i++) {
    if ((i & 0xf) == 0) {
      *dump << "\n  " << std::setfill('0') << std::setw(4) << std::hex << i
            << ": ";
    }
    *dump << std::setfill('0') << std::setw(2) << std::hex
          << static_cast<unsigned>(bytes[i]) << ' ';
  }
  *dump << std::dec << std::setfill(' ');
}

}  // namespace

PacketHandle PacketClone(PacketHandle src) {
  DCHECK(src != nullptr);
  return rte_pktmbuf_clone(src, src->pool);
}

PacketHandle PacketCopy(PacketHandle src) {
  DCHECK(src != nullptr);
  return rte_pktmbuf_copy(src, src->pool, 0, src->pkt_len);
}

namespace {

// The private area starts with the metadata; the scratchpad after it is
// the module's own and is deliberately left zeroed on the new packet.
void CopyMetadata(PacketHandle src, PacketHandle dst) {
  if (dst != nullptr) {
    std::memcpy(rte_mbuf_to_priv(dst), rte_mbuf_to_priv(src), SNBUF_METADATA);
  }
}

}  // namespace

PacketHandle PacketCloneWithMetadata(PacketHandle src) {
  PacketHandle dst = PacketClone(src);
  CopyMetadata(src, dst);
  return dst;
}

PacketHandle PacketCopyWithMetadata(PacketHandle src) {
  PacketHandle dst = PacketCopy(src);
  CopyMetadata(src, dst);
  return dst;
}

namespace {

// A packet PacketCopyBulkWithMetadata copies from the bulk allocation: one
// segment with bytes (rte_pktmbuf_copy copies no empty packet), from `pool`.
bool CopiesFromBulk(PacketHandle src, const rte_mempool *pool) {
  return src->nb_segs == 1 && src->pkt_len != 0 && src->pool == pool;
}

// rte_pktmbuf_copy of a one-segment `src` into `dst`, fresh from
// rte_pktmbuf_alloc_bulk (rte_pktmbuf_alloc's reset), plus the metadata:
// the header fields __rte_pktmbuf_copy_hdr copies, ol_flags without the
// indirect and external bits, and the bytes at dst's default headroom. A
// packet longer than that room gets PacketCopyWithMetadata's chain instead,
// and `dst` is freed.
PacketHandle CopyLinearInto(PacketHandle src, PacketHandle dst) {
  const uint32_t len = src->pkt_len;
  if (unlikely(len > rte_pktmbuf_tailroom(dst))) {
    rte_pktmbuf_free(dst);
    return PacketCopyWithMetadata(src);
  }
  __rte_pktmbuf_copy_hdr(dst, src);
  dst->ol_flags = src->ol_flags & ~(RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL);
  utils::CopyInlined(rte_pktmbuf_mtod(dst, void *),
                     rte_pktmbuf_mtod(src, const void *), len);
  dst->data_len = static_cast<uint16_t>(len);
  dst->pkt_len = len;
  CopyMetadata(src, dst);
  return dst;
}

}  // namespace

void PacketCopyBulkWithMetadata(const PacketHandle *srcs, size_t cnt,
                                size_t copies, PacketHandle *dsts) {
  if (cnt == 0 || copies == 0) {
    return;
  }
  DCHECK(srcs != nullptr && dsts != nullptr);
  rte_mempool *const pool = srcs[0]->pool;
  size_t from_bulk = 0;
  for (size_t i = 0; i < cnt; i++) {
    if (CopiesFromBulk(srcs[i], pool)) {
      from_bulk++;
    }
  }

  // The bulk fills dsts' last slots. Packet i's copies land at i * copies
  // on, never past the bulk slots not yet taken, so each bulk slot is read
  // before a copy overwrites it.
  const size_t total = cnt * copies;
  const size_t bulk_count = from_bulk * copies;
  PacketHandle *bulk = dsts + (total - bulk_count);
  if (bulk_count == 0 || bulk_count > std::numeric_limits<unsigned>::max() ||
      rte_pktmbuf_alloc_bulk(pool, bulk, static_cast<unsigned>(bulk_count)) !=
          0) {
    for (size_t slot = 0; slot < total; slot++) {
      dsts[slot] = PacketCopyWithMetadata(srcs[slot / copies]);
    }
    return;
  }
  for (size_t i = 0; i < cnt; i++) {
    PacketHandle src = srcs[i];
    PacketHandle *out = dsts + i * copies;
    const bool bulk_src = CopiesFromBulk(src, pool);
    for (size_t k = 0; k < copies; k++) {
      if (bulk_src) {
        PacketHandle fresh = *bulk++;
        out[k] = CopyLinearInto(src, fresh);
      } else {
        out[k] = PacketCopyWithMetadata(src);
      }
    }
  }
}

std::string PacketRef::Dump() const {
  std::ostringstream dump;
  const struct rte_mbuf *pkt = pkt_;

  dump << "dump packet at " << pkt << ", phys=" << rte_mbuf_iova_get(pkt)
       << ", buf_len=" << pkt->buf_len << '\n';
  dump << "  pkt_len=" << pkt->pkt_len << ", ol_flags=" << std::hex
       << pkt->ol_flags << ", nb_segs=" << std::dec << pkt->nb_segs
       << ", port=" << pkt->port << '\n';

  dump << "  refcnt chain: ";
  for (const struct rte_mbuf *seg = pkt; seg != nullptr; seg = seg->next) {
    dump << rte_mbuf_refcnt_read(seg) << ' ';
  }
  dump << '\n';

  dump << "  pool chain: ";
  for (const struct rte_mbuf *seg = pkt; seg != nullptr; seg = seg->next) {
    dump << seg->pool << ' ';
  }
  dump << '\n';

  uint32_t remaining = pkt->pkt_len;
  uint16_t segments = pkt->nb_segs;
  for (const struct rte_mbuf *seg = pkt;
       seg != nullptr && segments != 0; seg = seg->next, segments--) {
    const uint16_t segment_len = static_cast<uint16_t>(
        std::min<uint32_t>(remaining, static_cast<uint32_t>(seg->data_len)));
    dump << "  segment at " << seg << ", data="
         << rte_pktmbuf_mtod(seg, const void *)
         << ", data_len=" << std::dec << seg->data_len << '\n';
    HexDump(&dump, rte_pktmbuf_mtod(seg, const void *), segment_len);
    remaining -= segment_len;
  }

  return dump.str();
}

void PacketRef::CheckSanity() const { rte_mbuf_sanity_check(pkt_, 1); }

}  // namespace bess
