// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PACKET_HANDLE_H_
#define BESS_PACKET_HANDLE_H_

struct rte_mbuf;

namespace bess {

// What is *stored and transported* for a packet: in batches, rings, queues and
// ports. Packet-processing code should use PacketRef (packet.h) instead, which
// is the non-owning view over one of these.
//
// Stage 2B uses the native DPDK representation directly. PacketBatch arrays
// can therefore cross the PMD RX/TX boundary without a conversion loop.
using PacketHandle = struct rte_mbuf *;

}  // namespace bess

#endif  // BESS_PACKET_HANDLE_H_
