// SPDX-License-Identifier: BSD-3-Clause

// Handoff (dataplane/handoff.h) and tunnel decapsulation (tunnel/tunnel.h).
//
// Handoff moves packet ownership between workers with a bounded,
// fail-explicit channel: TryPunt either takes the packet (the caller's handle
// becomes nullptr) or leaves it with the caller and says why (kFull,
// kClosed). The consumer owns every packet it dequeues. A context travels
// with each packet; it must be trivially copyable and fit with the packet in
// one cache line (checked at compile time).
//
// Tunnel decapsulation is a pure function of the frame: it reports where the
// inner frame starts and the tunnel id, or a DecapError -- it never modifies
// the packet.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include <rte_mbuf.h>

#include "conntrack/packet_parse.h"
#include "dataplane/handoff.h"
#include "tunnel/tunnel.h"

namespace sample {

// What the producer already resolved, so the consumer does not reclassify.
struct Resolved {
  uint32_t action;
  uint32_t flow;
};

using Channel = bess::dataplane::HandoffChannel<Resolved>;

// `packet` is a packet this worker owns (an mbuf from bessd's pool), taken
// by reference: on success the channel nulls it, so the caller can see it no
// longer owns it.
bool HandoffSample(bess::PacketHandle &packet) {
  auto channel = Channel::Create({.capacity = 64});
  if (!channel) {
    return false;  // `packet` is still the caller's
  }
  if (auto punted = (*channel)->TryPunt(packet, Resolved{7, 42}); !punted) {
    // Still ours: the error says why (full or closed). This sample drops it.
    rte_pktmbuf_free(packet);
    packet = nullptr;
    return false;
  }
  // packet == nullptr: the channel owns it until the consumer dequeues it.
  std::array<Channel::Item, 8> items{};
  const size_t n = (*channel)->Dequeue(items);
  // The consumer owns items[0].packet now; this one is done with it.
  const bool ok = n == 1 && items[0].context.action == 7;
  for (size_t i = 0; i < n; i++) {
    rte_pktmbuf_free(items[i].packet);
  }
  return ok;
}

bool TunnelSample(std::span<const uint8_t> frame) {
  bess::conntrack::ParsedFlowPacket outer;
  bess::tunnel::Decapsulated inner;
  const bess::tunnel::DecapError error = bess::tunnel::DecapVxlan(frame, outer, inner);
  if (error != bess::tunnel::DecapError::kOk) {
    return false;  // not VXLAN, truncated, or malformed: the error says which
  }
  // inner.inner_offset: where the inner Ethernet frame starts; inner.id: VNI.
  return inner.inner_offset < frame.size();
}

}  // namespace sample
