// SPDX-License-Identifier: BSD-3-Clause

// Stateful batteries on one parsed packet: connection tracking
// (conntrack/conntrack.h) and NAT (nat/nat.h). Both are worker-owned
// libraries: one worker creates, tracks and expires; nothing is shared or
// locked. Packets are parsed once (conntrack::ParseFrame) and the parse is
// handed to each library. Failures are values: a create error, a parse status,
// a track status, a NAT verdict.

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

#include "conntrack/conntrack.h"
#include "conntrack/packet_parse.h"
#include "nat/nat.h"

namespace sample {

namespace ct = bess::conntrack;

// Ethernet + IPv4 + UDP, 10.0.0.1:1000 -> 192.0.2.1:53, no payload, padded
// to Ethernet's 60-byte minimum.
std::array<uint8_t, 60> UdpFrame() {
  std::array<uint8_t, 60> f{};
  f[12] = 0x08;                        // EtherType IPv4
  f[14] = 0x45;                        // IPv4, 20-byte header
  f[17] = 28;                          // total length
  f[22] = 64;                          // TTL
  f[23] = 17;                          // UDP
  f[26] = 10, f[29] = 1;               // source 10.0.0.1
  f[30] = 192, f[32] = 2, f[33] = 1;   // destination 192.0.2.1
  f[34] = 0x03, f[35] = 0xe8;          // source port 1000
  f[37] = 53;                          // destination port 53
  f[39] = 8;                           // UDP length
  return f;
}

bool StatefulSample(uint64_t now) {
  std::array<uint8_t, 60> frame = UdpFrame();
  ct::ParsedFlowPacket parsed;
  if (ct::ParseFrame(frame, parsed) != ct::ParseStatus::kOk) {
    return false;
  }

  // Connection tracking: the first packet creates the connection.
  auto tracker = ct::Conntrack<>::Create(/*capacity=*/1024);
  if (!tracker) {
    return false;
  }
  const auto first = (*tracker)->Track(frame, parsed, now);
  if (first.status != ct::TrackStatus::kNew) {
    return false;
  }
  // Expiry is the caller's loop: budgeted, never on its own.
  (void)(*tracker)->Expire(now + 1'000'000'000, /*budget=*/64);

  // NAT: one external address, a port range; outbound packets get a mapping.
  bess::nat::Nat::Config config;
  config.addresses.push_back({.addr = bess::utils::be32_t(0xcb007101),  // 203.0.113.1
                              .ranges = {{.begin = 1024, .end = 65536}}});
  config.capacity = 1024;
  auto nat = bess::nat::Nat::Create(config);
  if (!nat) {
    std::fprintf(stderr, "nat create failed: %d\n", static_cast<int>(nat.error()));
    return false;
  }
  const auto verdict = (*nat)->Translate(frame, parsed, bess::nat::Direction::kForward, now);
  // The frame now carries 203.0.113.1 and a pool port, checksums updated.
  return verdict == bess::nat::Verdict::kTranslated;
}

}  // namespace sample
