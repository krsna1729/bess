// SPDX-License-Identifier: BSD-3-Clause

// Test packets, layer by layer, as the Python module tests built them with
// scapy: lengths, EtherTypes, IP protocols and checksums are filled in at
// Build() from the layers that follow. Checksums are computed here (RFC 1071),
// not with utils/checksum.h, so a test never trusts the code it tests.
//
//   Bytes p = PacketBuilder().Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
//                 .IPv4("10.0.0.1", "10.0.0.2").Udp(1000, 53).Payload("hi").Build();
//
// A field left at its default takes scapy's default (IPv4 ttl 64, id 1; TCP
// flags SYN, window 8192; UDP and TCP checksums computed).

#ifndef BESS_TESTING_PACKET_BUILDER_H_
#define BESS_TESTING_PACKET_BUILDER_H_

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bess::testing {

using Bytes = std::vector<uint8_t>;

// "aa:bb:cc:dd:ee:ff" -> 6 bytes; aborts on a malformed address (a test bug).
inline std::array<uint8_t, 6> Mac(std::string_view text) {
  std::array<uint8_t, 6> out{};
  unsigned v[6];
  const std::string s(text);
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) !=
      6) {
    std::fprintf(stderr, "bad MAC address in a test: %s\n", s.c_str());
    std::abort();
  }
  for (int i = 0; i < 6; i++) {
    out[i] = static_cast<uint8_t>(v[i]);
  }
  return out;
}

inline std::array<uint8_t, 4> Ip4(std::string_view text) {
  std::array<uint8_t, 4> out{};
  if (inet_pton(AF_INET, std::string(text).c_str(), out.data()) != 1) {
    std::fprintf(stderr, "bad IPv4 address in a test: %.*s\n", static_cast<int>(text.size()),
                 text.data());
    std::abort();
  }
  return out;
}

inline std::array<uint8_t, 16> Ip6(std::string_view text) {
  std::array<uint8_t, 16> out{};
  if (inet_pton(AF_INET6, std::string(text).c_str(), out.data()) != 1) {
    std::fprintf(stderr, "bad IPv6 address in a test: %.*s\n", static_cast<int>(text.size()),
                 text.data());
    std::abort();
  }
  return out;
}

// The one's-complement sum of `data` added to `sum` (not folded).
inline uint32_t OnesSum(std::span<const uint8_t> data, uint32_t sum = 0) {
  size_t i = 0;
  for (; i + 1 < data.size(); i += 2) {
    sum += (uint32_t{data[i]} << 8) | data[i + 1];
  }
  if (i < data.size()) {
    sum += uint32_t{data[i]} << 8;
  }
  return sum;
}

inline uint16_t FoldChecksum(uint32_t sum) {
  while (sum >> 16) {
    sum = (sum & 0xffff) + (sum >> 16);
  }
  return static_cast<uint16_t>(~sum & 0xffff);
}

class PacketBuilder {
 public:
  struct EtherHdr {
    std::array<uint8_t, 6> dst, src;
    std::optional<uint16_t> type;
  };
  struct VlanHdr {  // 802.1Q (0x8100) or 802.1ad (0x88a8)
    uint16_t tpid;
    uint16_t vid;
    uint8_t pcp;
    std::optional<uint16_t> type;
  };
  struct IPv4Hdr {
    std::array<uint8_t, 4> src, dst;
    std::optional<uint8_t> proto;
    uint8_t ttl = 64;
    uint8_t tos = 0;
    uint16_t id = 1;
    uint16_t frag = 0;  // flags and fragment offset, as on the wire
    std::optional<uint16_t> checksum;  // set: written as given (a bad one)
    Bytes options;                     // a multiple of 4 bytes
  };
  struct IPv6Hdr {
    std::array<uint8_t, 16> src, dst;
    std::optional<uint8_t> next;
    uint8_t hlim = 64;
  };
  struct TcpHdr {
    uint16_t sport, dport;
    uint8_t flags = 0x02;  // SYN
    uint32_t seq = 0, ack = 0;
    uint16_t window = 8192;
    std::optional<uint16_t> checksum;
  };
  struct UdpHdr {
    uint16_t sport, dport;
    std::optional<uint16_t> checksum;
  };
  struct IcmpHdr {
    uint8_t type = 8, code = 0;
    uint16_t id = 0, seq = 0;
  };
  struct ArpHdr {
    uint16_t op;  // 1 request, 2 reply
    std::array<uint8_t, 6> hwsrc, hwdst;
    std::array<uint8_t, 4> psrc, pdst;
  };
  struct VxlanHdr {
    uint32_t vni;
  };
  struct Raw {
    Bytes data;
  };
  using Layer = std::variant<EtherHdr, VlanHdr, IPv4Hdr, IPv6Hdr, TcpHdr, UdpHdr, IcmpHdr, ArpHdr, VxlanHdr, Raw>;

  PacketBuilder &Ether(std::string_view dst, std::string_view src,
                       std::optional<uint16_t> type = std::nullopt) {
    return Add(EtherHdr{Mac(dst), Mac(src), type});
  }
  PacketBuilder &Dot1Q(uint16_t vid, uint8_t pcp = 0) {
    return Add(VlanHdr{0x8100, vid, pcp, std::nullopt});
  }
  PacketBuilder &Dot1AD(uint16_t vid, uint8_t pcp = 0) {
    return Add(VlanHdr{0x88a8, vid, pcp, std::nullopt});
  }
  PacketBuilder &IPv4(std::string_view src, std::string_view dst) {
    return Add(IPv4Hdr{.src = Ip4(src), .dst = Ip4(dst)});
  }
  // A layer with every field given.
  PacketBuilder &Add(Layer layer) {
    layers_.push_back(std::move(layer));
    return *this;
  }
  PacketBuilder &IPv6(std::string_view src, std::string_view dst) {
    return Add(IPv6Hdr{.src = Ip6(src), .dst = Ip6(dst)});
  }
  PacketBuilder &Tcp(uint16_t sport, uint16_t dport, uint8_t flags = 0x02) {
    return Add(TcpHdr{.sport = sport, .dport = dport, .flags = flags});
  }
  PacketBuilder &Udp(uint16_t sport, uint16_t dport) {
    return Add(UdpHdr{.sport = sport, .dport = dport});
  }
  PacketBuilder &Icmp(uint8_t type = 8, uint8_t code = 0) {
    return Add(IcmpHdr{.type = type, .code = code});
  }
  PacketBuilder &Arp(uint16_t op, std::string_view hwsrc, std::string_view psrc,
                     std::string_view hwdst, std::string_view pdst) {
    return Add(ArpHdr{op, Mac(hwsrc), Mac(hwdst), Ip4(psrc), Ip4(pdst)});
  }
  PacketBuilder &Vxlan(uint32_t vni) { return Add(VxlanHdr{vni}); }
  PacketBuilder &Payload(std::string_view text) {
    return Add(Raw{Bytes(text.begin(), text.end())});
  }
  PacketBuilder &Payload(Bytes data) { return Add(Raw{std::move(data)}); }
  // Pads with '0' characters (as get_tcp_packet did) to `len` bytes in all.
  PacketBuilder &PadTo(size_t len) {
    pad_to_ = len;
    return *this;
  }

  // The last layer of type T, to change a field after a helper made it.
  template <typename T>
  T &Last() {
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
      if (auto *l = std::get_if<T>(&*it)) {
        return *l;
      }
    }
    std::fprintf(stderr, "PacketBuilder::Last: no such layer\n");
    std::abort();
  }

  Bytes Build() const {
    size_t total = 0;
    for (const Layer &l : layers_) {
      total += HeaderSize(l);
    }
    const size_t pad = pad_to_ > total ? pad_to_ - total : 0;
    // From the innermost layer out, so each knows what follows it.
    Bytes tail(pad, '0');
    for (size_t i = layers_.size(); i-- > 0;) {
      const Layer *next = i + 1 < layers_.size() ? &layers_[i + 1] : nullptr;
      const Layer *prev = i > 0 ? &layers_[i - 1] : nullptr;
      Bytes header = Encode(layers_[i], next, prev, tail);
      header.insert(header.end(), tail.begin(), tail.end());
      tail = std::move(header);
    }
    return tail;
  }

 private:
  static void Put16(Bytes &b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
  }
  static void Put32(Bytes &b, uint32_t v) {
    Put16(b, static_cast<uint16_t>(v >> 16));
    Put16(b, static_cast<uint16_t>(v));
  }
  static void Set16(Bytes &b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
  }

  static size_t HeaderSize(const Layer &l) {
    return std::visit(
        [](const auto &x) -> size_t {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, EtherHdr>) return 14;
          if constexpr (std::is_same_v<T, VlanHdr>) return 4;
          if constexpr (std::is_same_v<T, IPv4Hdr>) return 20 + x.options.size();
          if constexpr (std::is_same_v<T, IPv6Hdr>) return 40;
          if constexpr (std::is_same_v<T, TcpHdr>) return 20;
          if constexpr (std::is_same_v<T, UdpHdr>) return 8;
          if constexpr (std::is_same_v<T, IcmpHdr>) return 8;
          if constexpr (std::is_same_v<T, ArpHdr>) return 28;
          if constexpr (std::is_same_v<T, VxlanHdr>) return 8;
          if constexpr (std::is_same_v<T, Raw>) return x.data.size();
        },
        l);
  }

  // The EtherType that announces `next` (a Vlan's TPID, IPv4, IPv6, ARP).
  static uint16_t EtherTypeOf(const Layer *next) {
    if (next == nullptr) return 0x9000;  // scapy's default: loopback
    if (auto *v = std::get_if<VlanHdr>(next)) return v->tpid;
    if (std::holds_alternative<IPv4Hdr>(*next)) return 0x0800;
    if (std::holds_alternative<IPv6Hdr>(*next)) return 0x86dd;
    if (std::holds_alternative<ArpHdr>(*next)) return 0x0806;
    if (std::holds_alternative<EtherHdr>(*next)) return 0x6558;  // transparent bridging
    return 0x9000;
  }
  static uint8_t ProtoOf(const Layer *next) {
    if (next == nullptr) return 0;
    if (std::holds_alternative<TcpHdr>(*next)) return 6;
    if (std::holds_alternative<UdpHdr>(*next)) return 17;
    if (std::holds_alternative<IcmpHdr>(*next)) return 1;
    if (std::holds_alternative<IPv4Hdr>(*next)) return 4;
    if (std::holds_alternative<IPv6Hdr>(*next)) return 41;
    return 0;
  }

  // The pseudo-header sum of the IP layer `prev` for an upper layer of
  // `length` bytes and protocol `proto`.
  static uint32_t PseudoSum(const Layer *prev, size_t length, uint8_t proto) {
    uint32_t sum = 0;
    if (auto *ip = prev ? std::get_if<IPv4Hdr>(prev) : nullptr) {
      sum = OnesSum(ip->src, sum);
      sum = OnesSum(ip->dst, sum);
      sum += proto;
      sum += static_cast<uint32_t>(length);
    } else if (auto *ip6 = prev ? std::get_if<IPv6Hdr>(prev) : nullptr) {
      sum = OnesSum(ip6->src, sum);
      sum = OnesSum(ip6->dst, sum);
      sum += static_cast<uint32_t>(length >> 16) + static_cast<uint32_t>(length & 0xffff);
      sum += proto;
    }
    return sum;
  }

  // `l`'s header; `tail` is everything after it, already encoded.
  static Bytes Encode(const Layer &l, const Layer *next, const Layer *prev, const Bytes &tail) {
    Bytes b;
    if (auto *e = std::get_if<EtherHdr>(&l)) {
      b.insert(b.end(), e->dst.begin(), e->dst.end());
      b.insert(b.end(), e->src.begin(), e->src.end());
      Put16(b, e->type.value_or(EtherTypeOf(next)));
    } else if (auto *v = std::get_if<VlanHdr>(&l)) {
      Put16(b, static_cast<uint16_t>((v->pcp << 13) | (v->vid & 0x0fff)));
      Put16(b, v->type.value_or(EtherTypeOf(next)));
      // The TPID is the previous layer's EtherType (EtherTypeOf).
    } else if (auto *ip = std::get_if<IPv4Hdr>(&l)) {
      const size_t ihl = 20 + ip->options.size();
      b.push_back(static_cast<uint8_t>(0x40 | (ihl / 4)));
      b.push_back(ip->tos);
      Put16(b, static_cast<uint16_t>(ihl + tail.size()));
      Put16(b, ip->id);
      Put16(b, ip->frag);
      b.push_back(ip->ttl);
      b.push_back(ip->proto.value_or(ProtoOf(next)));
      Put16(b, 0);
      b.insert(b.end(), ip->src.begin(), ip->src.end());
      b.insert(b.end(), ip->dst.begin(), ip->dst.end());
      b.insert(b.end(), ip->options.begin(), ip->options.end());
      Set16(b, 10, ip->checksum.value_or(FoldChecksum(OnesSum(b))));
    } else if (auto *ip6 = std::get_if<IPv6Hdr>(&l)) {
      Put32(b, 0x60000000);
      Put16(b, static_cast<uint16_t>(tail.size()));
      b.push_back(ip6->next.value_or(ProtoOf(next)));
      b.push_back(ip6->hlim);
      b.insert(b.end(), ip6->src.begin(), ip6->src.end());
      b.insert(b.end(), ip6->dst.begin(), ip6->dst.end());
    } else if (auto *t = std::get_if<TcpHdr>(&l)) {
      Put16(b, t->sport);
      Put16(b, t->dport);
      Put32(b, t->seq);
      Put32(b, t->ack);
      b.push_back(5 << 4);
      b.push_back(t->flags);
      Put16(b, t->window);
      Put16(b, 0);
      Put16(b, 0);
      const uint32_t sum = OnesSum(tail, OnesSum(b, PseudoSum(prev, b.size() + tail.size(), 6)));
      Set16(b, 16, t->checksum.value_or(FoldChecksum(sum)));
    } else if (auto *u = std::get_if<UdpHdr>(&l)) {
      Put16(b, u->sport);
      Put16(b, u->dport);
      Put16(b, static_cast<uint16_t>(8 + tail.size()));
      Put16(b, 0);
      uint16_t c = FoldChecksum(OnesSum(tail, OnesSum(b, PseudoSum(prev, 8 + tail.size(), 17))));
      if (c == 0) c = 0xffff;
      Set16(b, 6, u->checksum.value_or(c));
    } else if (auto *ic = std::get_if<IcmpHdr>(&l)) {
      b.push_back(ic->type);
      b.push_back(ic->code);
      Put16(b, 0);
      Put16(b, ic->id);
      Put16(b, ic->seq);
      Set16(b, 2, FoldChecksum(OnesSum(tail, OnesSum(b))));
    } else if (auto *a = std::get_if<ArpHdr>(&l)) {
      Put16(b, 1);
      Put16(b, 0x0800);
      b.push_back(6);
      b.push_back(4);
      Put16(b, a->op);
      b.insert(b.end(), a->hwsrc.begin(), a->hwsrc.end());
      b.insert(b.end(), a->psrc.begin(), a->psrc.end());
      b.insert(b.end(), a->hwdst.begin(), a->hwdst.end());
      b.insert(b.end(), a->pdst.begin(), a->pdst.end());
    } else if (auto *x = std::get_if<VxlanHdr>(&l)) {
      Put32(b, 0x08000000);
      Put32(b, x->vni << 8);
    } else if (auto *r = std::get_if<Raw>(&l)) {
      b = r->data;
    }
    return b;
  }

  std::vector<Layer> layers_;
  size_t pad_to_ = 0;
};

// get_tcp_packet / get_udp_packet: random MACs, the given (or random)
// addresses and ports, padded to `len`. `rng` makes them reproducible.
inline std::string RandomIp(std::mt19937 &rng) {
  std::uniform_int_distribution<int> octet(1, 254);
  return std::to_string(octet(rng)) + "." + std::to_string(octet(rng)) + "." +
         std::to_string(octet(rng)) + "." + std::to_string(octet(rng));
}

inline std::string RandomMac(std::mt19937 &rng) {
  std::uniform_int_distribution<int> byte(0, 255);
  char buf[18];
  std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", byte(rng) & 0xfe, byte(rng),
                byte(rng), byte(rng), byte(rng), byte(rng));
  return buf;
}

struct FlowSpec {
  std::string sip, dip;  // empty: random
  int sport = -1, dport = -1;  // -1: random
  size_t len = 60;
};

inline Bytes TcpPacket(const FlowSpec &f, std::mt19937 &rng) {
  std::uniform_int_distribution<int> port(0, 65535);
  return PacketBuilder()
      .Ether(RandomMac(rng), RandomMac(rng))
      .IPv4(f.sip.empty() ? RandomIp(rng) : f.sip, f.dip.empty() ? RandomIp(rng) : f.dip)
      .Tcp(static_cast<uint16_t>(f.sport < 0 ? port(rng) : f.sport),
           static_cast<uint16_t>(f.dport < 0 ? port(rng) : f.dport))
      .PadTo(f.len)
      .Build();
}

inline Bytes UdpPacket(const FlowSpec &f, std::mt19937 &rng) {
  std::uniform_int_distribution<int> port(0, 65535);
  return PacketBuilder()
      .Ether(RandomMac(rng), RandomMac(rng))
      .IPv4(f.sip.empty() ? RandomIp(rng) : f.sip, f.dip.empty() ? RandomIp(rng) : f.dip)
      .Udp(static_cast<uint16_t>(f.sport < 0 ? port(rng) : f.sport),
           static_cast<uint16_t>(f.dport < 0 ? port(rng) : f.dport))
      .PadTo(f.len)
      .Build();
}

}  // namespace bess::testing

#endif  // BESS_TESTING_PACKET_BUILDER_H_
