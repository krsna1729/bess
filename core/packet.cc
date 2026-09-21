#include "packet.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

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
