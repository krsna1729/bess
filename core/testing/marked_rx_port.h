// SPDX-License-Identifier: BSD-3-Clause

// A port whose receive hands out packets carrying the RX MARK state a PMD
// leaves in the mbuf (D-185): RTE_MBUF_F_RX_FDIR_ID with the MARK in
// hash.fdir.hi on some, and on others no flags, or a value in hash.fdir.hi
// without RTE_MBUF_F_RX_FDIR_ID (a flow match without an ID, or a stale
// field), which is not a MARK. A test registers it with ADD_DRIVER.

#ifndef BESS_TESTING_MARKED_RX_PORT_H_
#define BESS_TESTING_MARKED_RX_PORT_H_

#include <rte_mbuf.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "gate.h"
#include "packet.h"
#include "packet_pool.h"
#include "port.h"
#include "testing/packet_builder.h"
#include "worker.h"

namespace bess::testing {

struct MarkedRx {
  uint64_t ol_flags;
  uint32_t fdir_hi;
  uint32_t mark;  // what a reader of the MARK attribute must see
};

// RTE_MBUF_F_RX_FDIR and RTE_MBUF_F_RX_FDIR_ID, once: clang-tidy's
// bugprone-signed-bitwise reports DPDK's int shift counts at each expansion.
inline constexpr uint64_t kRxFdir = RTE_MBUF_F_RX_FDIR;       // NOLINT(bugprone-signed-bitwise)
inline constexpr uint64_t kRxFdirId = RTE_MBUF_F_RX_FDIR_ID;  // NOLINT(bugprone-signed-bitwise)

// One receive's packets, in order. The MARK values are distinct gates of a
// Split on the 4-byte attribute; 258 (0x0102) and 4097 (0x1001) read as other
// values when the bytes are swapped.
inline const std::vector<MarkedRx> &MarkedRxPackets() {
  static const std::vector<MarkedRx> packets = {
      {kRxFdir | kRxFdirId, 1, 1},
      {0, 0, 0},
      {kRxFdir | kRxFdirId, 258, 258},
      {kRxFdir, 7, 0},
      {kRxFdir | kRxFdirId, 4097, 4097},
      {0, 7, 0},
  };
  return packets;
}

// Packet i of a receive: 60 zero bytes with i in the first.
inline Bytes MarkedRxBytes(size_t i) {
  Bytes bytes(60, 0);
  bytes[0] = static_cast<uint8_t>(i);
  return bytes;
}

// The gates of a Split on the MARK attribute: every MARK, and 7, where a
// value read without RTE_MBUF_F_RX_FDIR_ID would go.
inline std::vector<gate_idx_t> MarkedRxGates() { return {0, 1, 7, 258, 4097}; }

// What each of MarkedRxGates() receives from one receive.
inline std::map<gate_idx_t, std::vector<Bytes>> MarkedRxExpected() {
  std::map<gate_idx_t, std::vector<Bytes>> out;
  for (size_t i = 0; i < MarkedRxPackets().size(); i++) {
    out[static_cast<gate_idx_t>(MarkedRxPackets()[i].mark)].push_back(MarkedRxBytes(i));
  }
  return out;
}

class MarkedRxPort final : public Port {
 public:
  CommandResponse Init(const google::protobuf::Any &) { return CommandSuccess(); }
  void DeInit() override {}
  int RecvPackets(queue_t, bess::PacketHandle *pkts, int cnt) override {
    const int n = std::min(cnt, static_cast<int>(MarkedRxPackets().size()));
    if (n == 0 || !current_worker.packet_pool()->AllocBulk(pkts, n, 60)) {
      return 0;
    }
    for (int i = 0; i < n; i++) {
      rte_mbuf *m = pkts[i];
      const Bytes bytes = MarkedRxBytes(static_cast<size_t>(i));
      std::memcpy(rte_pktmbuf_mtod(m, uint8_t *), bytes.data(), bytes.size());
      // Metadata a write skipped would leave reads 0xa5a5a5a5, not 0.
      std::memset(bess::PacketRef(m).metadata<char *>(), 0xa5, SNBUF_METADATA);
      m->ol_flags = MarkedRxPackets()[i].ol_flags;
      m->hash.fdir.hi = MarkedRxPackets()[i].fdir_hi;
    }
    return n;
  }
  int SendPackets(queue_t, bess::PacketHandle *, int) override { return 0; }
};

}  // namespace bess::testing

#endif  // BESS_TESTING_MARKED_RX_PORT_H_
