// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_TESTING_PARSE_CORPUS_H_
#define BESS_TESTING_PARSE_CORPUS_H_

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "testing/packet_builder.h"

namespace bess::testing {

// One frame for a reader of packet::ParseFrame, with the packet length its
// parse is given (more than the frame's for a chained packet).
struct ParseCorpusFrame {
  Bytes bytes;
  size_t total_len;
};

// Frames for differential tests of the parse's readers (D-188): valid
// untagged IPv4 TCP, UDP, ICMP and another protocol, IPv4 with options, first
// and later fragments, VLAN and QinQ, IPv6 with and without an extension
// header; each also truncated at every length, with every one of its first 64
// bytes replaced (0x00, 0xff and a random value), and as the head of a longer
// packet; and random frames, half of them claiming untagged IPv4 without
// options. Lengths that lie, IHL and version changes and bad TCP and UDP
// lengths all come out of the byte replacements.
inline std::vector<ParseCorpusFrame> ParseCorpus(uint32_t seed = 1) {
  auto ether = [] {
    PacketBuilder b;
    b.Ether("02:00:00:00:00:01", "02:00:00:00:00:02");
    return b;
  };
  using V4 = PacketBuilder::IPv4Hdr;
  using V6 = PacketBuilder::IPv6Hdr;
  const std::vector<Bytes> bases = {
      ether().IPv4("10.0.0.1", "10.1.0.1").Tcp(1000, 80).PadTo(64).Build(),
      ether().IPv4("10.0.0.1", "10.1.0.1").Udp(1000, 53).Payload("abcdefgh").Build(),
      ether().IPv4("10.0.0.1", "10.1.0.1").Add(PacketBuilder::IcmpHdr{.id = 7}).Build(),
      ether()
          .Add(V4{.src = Ip4("10.0.0.1"), .dst = Ip4("10.1.0.1"), .proto = 47})
          .Payload("gre-ish payload")
          .Build(),
      ether()
          .Add(V4{.src = Ip4("10.0.0.1"), .dst = Ip4("10.1.0.1"), .options = Bytes(4, 1)})
          .Tcp(1000, 80)
          .Build(),
      ether()
          .Add(V4{.src = Ip4("10.0.0.1"), .dst = Ip4("10.1.0.1"), .proto = 17, .frag = 0x2000})
          .Udp(1000, 53)
          .Payload("first fragment")
          .Build(),
      ether()
          .Add(V4{.src = Ip4("10.0.0.1"), .dst = Ip4("10.1.0.1"), .proto = 6, .frag = 0x0004})
          .Payload("a later fragment's bytes")
          .Build(),
      ether().Dot1Q(7).IPv4("10.0.0.1", "10.1.0.1").Tcp(1000, 80).Build(),
      ether().Dot1AD(5).Dot1Q(7).IPv4("10.0.0.1", "10.1.0.1").Udp(1000, 53).Build(),
      ether().IPv6("2001:db8::1", "2001:db8:1::1").Tcp(1000, 80).Build(),
      ether()
          .Add(V6{.src = Ip6("2001:db8::1"), .dst = Ip6("2001:db8:1::1"), .next = 60})
          .Payload(Bytes{17, 0, 1, 4, 0, 0, 0, 0})
          .Udp(1000, 53)
          .Build(),
  };
  std::mt19937 rng(seed);
  auto byte = [&] { return static_cast<uint8_t>(rng()); };
  std::vector<ParseCorpusFrame> out;
  for (const Bytes &base : bases) {
    out.push_back({base, base.size()});
    out.push_back({base, base.size() + 100});  // the head of a chained packet
    for (size_t len = 0; len < base.size(); len++) {
      out.push_back({Bytes(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(len)), len});
    }
    for (size_t i = 0; i < base.size() && i < 64; i++) {
      for (const uint8_t v : {uint8_t{0x00}, uint8_t{0xff}, byte()}) {
        Bytes b = base;
        b[i] = v;
        out.push_back({b, b.size()});
      }
    }
  }
  for (int i = 0; i < 2000; i++) {
    Bytes b(14 + rng() % 70);
    for (uint8_t &x : b) {
      x = byte();
    }
    if (i % 2 == 0 && b.size() >= 15) {
      b[12] = 0x08;
      b[13] = 0x00;
      b[14] = 0x45;
    }
    out.push_back({b, b.size() + (i % 3 == 0 ? 50 : 0)});
  }
  return out;
}

}  // namespace bess::testing

#endif  // BESS_TESTING_PARSE_CORPUS_H_
