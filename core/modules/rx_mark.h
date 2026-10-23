// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_RX_MARK_H_
#define BESS_MODULES_RX_MARK_H_

#include <rte_mbuf.h>

#include <cstdint>

#include "metadata.h"
#include "module.h"
#include "packet.h"
#include "pktbatch.h"
#include "utils/endian.h"

namespace bess::modules {

// RTE_MBUF_F_RX_FDIR_ID, `1ULL << 13`: clang-tidy's bugprone-signed-bitwise
// reports the int shift count wherever the macro expands.
inline constexpr uint64_t kRxFdirId = RTE_MBUF_F_RX_FDIR_ID;  // NOLINT(bugprone-signed-bitwise)

// PortInc/QueueInc `mark_attr` (D-185): writes each received packet's rte_flow
// MARK to the 4-byte (be32) attribute at `offset`, or 0 when the PMD reported
// none. DPDK carries the MARK in hash.fdir.hi exactly when RTE_MBUF_F_RX_FDIR_ID
// is set. FlowRuleOwner never hands out 0 (mark_base >= 1), so 0 is "no mark".
// `offset` must be valid: the caller tests it once per batch.
inline void WriteRxMarks(bess::metadata::mt_offset_t offset, bess::PacketBatch *batch) {
  for (bess::PacketRef pkt : batch->packets()) {
    const rte_mbuf *m = pkt.handle();
    const uint32_t mark = (m->ol_flags & kRxFdirId) != 0 ? m->hash.fdir.hi : 0;
    _set_attr_with_offset<bess::utils::be32_t>(offset, pkt, bess::utils::be32_t(mark));
  }
}

}  // namespace bess::modules

#endif  // BESS_MODULES_RX_MARK_H_
