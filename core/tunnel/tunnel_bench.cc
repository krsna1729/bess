// SPDX-License-Identifier: BSD-3-Clause

// M19 (D-069): tunnel mechanics, per packet.
//
//   BM_VxlanEncap/<impl>  ns to write UDP + VXLAN headers in front of an inner
//     frame (the headroom is there): 0 the legacy VXLANEncap body (unchecked
//     inner parse, legacy hash), 1 FlowEntropyPort + WriteVxlanUdp
//   BM_Decap/<kind>       ns to check and locate the inner packet: 0 the legacy
//     VXLANDecap body (fixed offsets, no checks), 1 DecapVxlan, 2 DecapGeneve
//     (8 option bytes), 3 DecapGre (key), 4 DecapGtpu (sequence + one
//     extension header)

#include <benchmark/benchmark.h>

#include <rte_hash_crc.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "tunnel/tunnel.h"
#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/udp.h"
#include "utils/vxlan.h"

namespace {

using namespace bess::tunnel;
using bess::utils::be16_t;
using bess::utils::be32_t;

std::vector<uint8_t> Inner() {
  std::vector<uint8_t> f(kEthernetBytes + kIpv4Bytes + kUdpBytes + 64, 0);
  detail::Put16(f.data() + 12, 0x0800);
  WriteIpv4(f.data() + 14, be32_t(0x0a000001), be32_t(0x0a000002), 17, 8 + 64);
  WriteUdp(f.data() + 34, 1234, 80, 64);
  return f;
}

// The legacy VXLANEncap per-packet body (modules/vxlan_encap.cc before M19).
void LegacyEncap(uint8_t *headroom_end, uint32_t vni, be16_t dstport, size_t total_len) {
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Udp;
  using bess::utils::Vxlan;
  Ethernet *inner_eth = reinterpret_cast<Ethernet *>(headroom_end);
  const size_t inner_frame_len = total_len + sizeof(Udp);
  Udp *udp = reinterpret_cast<Udp *>(headroom_end - sizeof(Udp) - sizeof(Vxlan));
  Vxlan *vh = reinterpret_cast<Vxlan *>(udp + 1);
  vh->vx_flags = be32_t(0x08000000);
  vh->vx_vni = be32_t(vni) << 8;
  uint32_t h = 0;
  if (inner_eth->ether_type.value() != Ethernet::Type::kIpv4) {
    h = rte_hash_crc(inner_eth, sizeof(Ethernet::Address) * 2, UINT32_MAX);
  } else {
    Ipv4 *inner_ip = reinterpret_cast<Ipv4 *>(inner_eth + 1);
    size_t ip_len = inner_ip->header_length;
    h = inner_ip->protocol;
    if (inner_ip->protocol == Ipv4::Proto::kTcp || inner_ip->protocol == Ipv4::Proto::kUdp) {
      Udp *inner_l4 = reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(inner_ip) + ip_len);
      h = rte_hash_crc(&inner_l4->src_port, sizeof(be32_t), h);
    }
    h = rte_hash_crc(&inner_ip->src, sizeof(be32_t) * 2, h);
  }
  udp->src_port = be16_t(static_cast<uint16_t>(h | 0xc000));
  udp->dst_port = dstport;
  udp->length = be16_t(static_cast<uint16_t>(sizeof(*udp) + inner_frame_len));
  udp->checksum = 0;
}

void BM_VxlanEncap(benchmark::State &st) {
  const auto inner = Inner();
  std::vector<uint8_t> buf(64 + inner.size());
  std::memcpy(buf.data() + 64, inner.data(), inner.size());
  uint8_t *start = buf.data() + 64;
  uint32_t vni = 1;
  for (auto _ : st) {
    if (st.range(0) == 0) {
      LegacyEncap(start, vni++, be16_t(4789), inner.size());
    } else {
      const uint16_t port = FlowEntropyPort(std::span<const uint8_t>(start, inner.size()));
      WriteVxlanUdp(start - 16, vni++, port, kVxlanPort, inner.size());
    }
    benchmark::DoNotOptimize(buf.data());
    benchmark::ClobberMemory();
  }
}

std::vector<uint8_t> Outer(uint16_t dport, const std::vector<uint8_t> &tunnel_header,
                           const std::vector<uint8_t> &payload, uint8_t proto = 17) {
  std::vector<uint8_t> f(kEthernetBytes + kIpv4Bytes, 0);
  detail::Put16(f.data() + 12, 0x0800);
  const size_t l4 = (proto == 17 ? kUdpBytes : 0) + tunnel_header.size() + payload.size();
  WriteIpv4(f.data() + 14, be32_t(0xc0000201), be32_t(0xc0000202), proto, static_cast<uint16_t>(l4));
  if (proto == 17) {
    f.resize(f.size() + kUdpBytes);
    WriteUdp(f.data() + 34, 0xc000, dport, static_cast<uint16_t>(tunnel_header.size() + payload.size()));
  }
  f.insert(f.end(), tunnel_header.begin(), tunnel_header.end());
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

void BM_Decap(benchmark::State &st) {
  const auto inner = Inner();
  const std::vector<uint8_t> ip_inner(inner.begin() + 14, inner.end());
  std::vector<uint8_t> frame;
  switch (st.range(0)) {
    case 0:
    case 1: {
      std::vector<uint8_t> vx(8, 0);
      vx[0] = 0x08;
      detail::Put32(vx.data() + 4, 0x123400);
      frame = Outer(kVxlanPort, vx, inner);
      break;
    }
    case 2: {
      std::vector<uint8_t> g(16, 0);
      WriteGeneve(g.data(), 0x1234, 0x6558, 8);
      frame = Outer(kGenevePort, g, inner);
      break;
    }
    case 3: {
      GreOptions o;
      o.key = 7;
      std::vector<uint8_t> g(GreBytes(o));
      WriteGre(g.data(), 0x0800, o);
      frame = Outer(0, g, ip_inner, 47);
      break;
    }
    default: {
      std::vector<uint8_t> g = {0x36, 255, 0, 0, 0x11, 0x22, 0x33, 0x44, 0, 7, 0, 0x85, 1, 0x10, 0x09, 0};
      detail::Put16(g.data() + 2, static_cast<uint16_t>(8 + ip_inner.size()));
      frame = Outer(kGtpuPort, g, ip_inner);
      break;
    }
  }
  size_t sink = 0;
  bess::conntrack::ParsedFlowPacket outer;
  for (auto _ : st) {
    switch (st.range(0)) {
      case 0: {
        // The legacy VXLANDecap body: fixed offsets, no checks.
        const uint8_t *ip = frame.data() + 14;
        const size_t ip_bytes = static_cast<size_t>(ip[0] & 0x0f) << 2;
        const uint8_t *vh = ip + ip_bytes + 8;
        sink += detail::Get32(ip + 12) + detail::Get32(ip + 16) + (detail::Get32(vh + 4) >> 8);
        sink += 14 + ip_bytes + 8 + 8;
        break;
      }
      case 1: {
        Decapsulated d;
        sink += DecapVxlan(frame, outer, d) == DecapError::kOk ? d.inner_offset + d.id : 0;
        break;
      }
      case 2: {
        GeneveInfo d;
        sink += DecapGeneve(frame, outer, d) == DecapError::kOk ? d.tunnel.inner_offset : 0;
        break;
      }
      case 3: {
        Decapsulated d;
        sink += DecapGre(frame, outer, d) == DecapError::kOk ? d.inner_offset : 0;
        break;
      }
      default: {
        GtpuInfo d;
        sink += DecapGtpu(frame, outer, d) == DecapError::kOk ? d.tunnel.inner_offset : 0;
        break;
      }
    }
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  if (sink == 0) st.SkipWithError("decap failed");
}

BENCHMARK(BM_VxlanEncap)->Arg(0)->Arg(1)->MinTime(0.2);
BENCHMARK(BM_Decap)->DenseRange(0, 4)->MinTime(0.2);

}  // namespace
