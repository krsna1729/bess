// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// BpfProgram (DPDK rte_bpf) against libpcap's own classic-BPF interpreter,
// bpf_filter(), as the reference: the same verdict for every expression of a
// corpus on every packet of a generated corpus, whole or split across two
// segments. Decision D-018 (docs/decisions.md).

#include "utils/bpf_program.h"

#include <gtest/gtest.h>
#include <pcap.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using bess::utils::BpfProgram;

// Chosen to exercise every classic instruction class pcap emits for real
// filters: absolute and indirect loads of each width, the IP header-length
// load (ldxb 4*([k]&0xf)), ALU with constants and X, all jump kinds, the
// scratch memory, the packet length, and fragments.
const char *const kExpressions[] = {
    "ip",
    "ip6",
    "arp",
    "vlan",
    "vlan and ip",
    "tcp",
    "udp",
    "icmp",
    "not tcp",
    "tcp src port 92",
    "tcp dst port 80",
    "udp port 53",
    "port 443",
    "portrange 1000-2000",
    "ip host 22.22.22.22",
    "src net 10.0.0.0/8",
    "dst net 10.1.0.0/16 and udp",
    "ip proto 47 or ip6 proto 47",
    "ether proto 0x800",
    "ether host 02:00:00:00:00:01",
    "ether broadcast",
    "len <= 100",
    "len > 200",
    "greater 128",
    "ip[2:2] > 100",
    "ip[0] & 0xf != 5",
    "ip[6:2] & 0x1fff != 0",
    "tcp[tcpflags] & tcp-syn != 0",
    "tcp[13] & 0x12 = 0x12",
    "icmp[icmptype] = icmp-echo",
    "ip and (tcp or udp) and port 443",
    // clang-format off
    "tcp port 80 and (((ip[2:2] - ((ip[0]&0xf)<<2)) - ((tcp[12]&0xf0)>>2)) != 0)",
    // clang-format on
    "ip[2:2] * 3 > 600",
    "ip[2:2] / 4 = 25",
    "(ip[2:2] >> 2) + 1 = (len >> 3) - 1",
    "ip[1] | 0x0f = 0x0f",
    "ip[2:2] % (ip[9] & 0x4) = 1",  // divisor 0 for UDP/ICMP
    "ip[2:2] / (ip[9] & 0x4) > 10",
    "ip[8] - ip[9] < 60",
    "udp[8:4] = 0x01020304",
    "ip6 and tcp port 80",
};

// Deterministic packets that hit the corpus's branches: mostly well-formed
// Ethernet/VLAN, IPv4 (with and without options, sometimes fragmented),
// IPv6 and ARP with TCP/UDP/ICMP/GRE, fields from small value sets so filters
// both match and miss, then some bytes flipped and some packets truncated.
std::vector<std::vector<uint8_t>> MakePackets(size_t count) {
  std::mt19937 rng(0x5eed);
  auto pick = [&](std::initializer_list<uint32_t> values) {
    return *(values.begin() + rng() % values.size());
  };
  std::vector<std::vector<uint8_t>> packets;
  for (size_t n = 0; n < count; n++) {
    std::vector<uint8_t> p;
    auto put8 = [&](uint32_t v) { p.push_back(static_cast<uint8_t>(v)); };
    auto put16 = [&](uint32_t v) {
      put8(v >> 8);
      put8(v);
    };
    auto put32 = [&](uint32_t v) {
      put16(v >> 16);
      put16(v);
    };

    // Ethernet: dst, src, [VLAN], ethertype.
    const bool broadcast = rng() % 8 == 0;
    for (int i = 0; i < 6; i++) put8(broadcast ? 0xff : (i == 5 ? pick({1, 2}) : i == 0 ? 2 : 0));
    for (int i = 0; i < 6; i++) put8(i == 5 ? pick({1, 3}) : i == 0 ? 2 : 0);
    if (rng() % 6 == 0) {
      put16(0x8100);
      put16(rng() & 0xfff);
    }
    const uint32_t ethertype = pick({0x0800, 0x0800, 0x0800, 0x86dd, 0x0806,
                                     0x1234});
    put16(ethertype);

    const uint32_t proto = pick({6, 6, 17, 17, 1, 47, 99});
    const size_t l3 = p.size();
    if (ethertype == 0x0800) {
      const uint32_t ihl = rng() % 4 == 0 ? pick({6, 8, 15}) : 5;
      put8(0x40 | ihl);
      put8(pick({0, 0x10, 0xb8}));
      put16(0);  // total length, fixed below
      put16(rng());
      put16(rng() % 5 == 0 ? pick({0x2000, 0x0001, 0x00b9}) : 0x4000);
      put8(pick({1, 64, 255}));
      put8(proto);
      put16(rng());
      put32(pick({0x0a000001, 0x0a010203, 0x16161616, 0xc0a80001}));
      put32(pick({0x0a010001, 0x16161616, 0x08080808, 0x0a000002}));
      for (uint32_t i = 5; i < ihl; i++) put32(rng());
    } else if (ethertype == 0x86dd) {
      put32(0x60000000);
      put16(0);
      put8(proto);
      put8(64);
      for (int i = 0; i < 8; i++) put32(rng() % 3 == 0 ? 0 : rng());
    } else if (ethertype == 0x0806) {
      put16(1);
      put16(0x0800);
      put8(6);
      put8(4);
      put16(pick({1, 2}));
      for (int i = 0; i < 20; i++) put8(rng());
    }

    if (ethertype == 0x0800 || ethertype == 0x86dd) {
      if (proto == 6) {
        put16(pick({92, 80, 443, 1500, 3000, static_cast<uint32_t>(rng() & 0xffff)}));
        put16(pick({92, 80, 443, 1500, 3000, static_cast<uint32_t>(rng() & 0xffff)}));
        put32(rng());
        put32(rng());
        const uint32_t off = rng() % 4 == 0 ? 8 : 5;
        put8(off << 4);
        put8(pick({0x02, 0x12, 0x10, 0x18, 0x11}));
        put16(rng());
        put32(rng());
        for (uint32_t i = 5; i < off; i++) put32(rng());
      } else if (proto == 17) {
        put16(pick({53, 443, 1000, 2000, static_cast<uint32_t>(rng() & 0xffff)}));
        put16(pick({53, 443, 1234, static_cast<uint32_t>(rng() & 0xffff)}));
        put16(0);
        put16(0);
        put32(rng() % 3 == 0 ? 0x01020304 : rng());
      } else if (proto == 1) {
        put8(pick({8, 0, 3}));
        put8(0);
        put16(rng());
        put32(rng());
      }
    }
    const size_t payload = pick({0, 0, 4, 60, 200, 900});
    for (size_t i = 0; i < payload; i++) put8(rng());
    if (ethertype == 0x0800) {
      const size_t total = p.size() - l3;
      p[l3 + 2] = static_cast<uint8_t>(total >> 8);
      p[l3 + 3] = static_cast<uint8_t>(total);
    }

    // Damage: flip a few bytes, or truncate anywhere (even into Ethernet).
    if (rng() % 10 == 0) {
      for (int i = 0; i < 3; i++) p[rng() % p.size()] ^= 1u << (rng() % 8);
    }
    if (rng() % 10 == 0) {
      p.resize(rng() % p.size());
    }
    packets.push_back(std::move(p));
  }
  return packets;
}

// A packet as one or two mbufs over caller-owned bytes (no mempool needed:
// the BPF program reads only the data pointers, lengths and chain).
struct MbufChain {
  struct rte_mbuf seg[2];

  MbufChain(std::vector<uint8_t> &data, size_t split) {
    memset(seg, 0, sizeof(seg));
    const size_t first = split < data.size() ? split : data.size();
    Fill(&seg[0], data.data(), first);
    seg[0].pkt_len = static_cast<uint32_t>(data.size());
    seg[0].nb_segs = 1;
    if (first < data.size()) {
      Fill(&seg[1], data.data() + first, data.size() - first);
      seg[0].next = &seg[1];
      seg[0].nb_segs = 2;
    }
  }

  static void Fill(struct rte_mbuf *m, uint8_t *bytes, size_t len) {
    m->buf_addr = bytes;
    m->data_off = 0;
    m->buf_len = static_cast<uint16_t>(len);
    m->data_len = static_cast<uint16_t>(len);
  }
};

std::string Hex(const std::vector<uint8_t> &p) {
  std::string out;
  char byte[4];
  for (uint8_t b : p) {
    snprintf(byte, sizeof(byte), "%02x", b);
    out += byte;
  }
  return out;
}

struct Reference {
  struct bpf_program program;

  explicit Reference(const char *expression) {
    pcap_t *pcap = pcap_open_dead(DLT_EN10MB, 0xffff);
    EXPECT_EQ(0, pcap_compile(pcap, &program, expression, 1,
                              PCAP_NETMASK_UNKNOWN))
        << expression << ": " << pcap_geterr(pcap);
    pcap_close(pcap);
  }
  ~Reference() { pcap_freecode(&program); }

  bool Matches(const std::vector<uint8_t> &p) const {
    return bpf_filter(program.bf_insns, p.data(), p.size(), p.size()) != 0;
  }
};

TEST(BpfProgramTest, SameVerdictsAsLibpcapOnWholeAndSplitPackets) {
  std::vector<std::vector<uint8_t>> packets = MakePackets(4000);
  std::mt19937 rng(0xb9f);

  for (const char *expression : kExpressions) {
    SCOPED_TRACE(expression);
    std::string error;
    std::unique_ptr<BpfProgram> program = BpfProgram::Compile(expression, &error);
    ASSERT_NE(nullptr, program) << error;
    const Reference reference(expression);

    size_t matched = 0;
    for (size_t i = 0; i < packets.size(); i++) {
      const bool expected = reference.Matches(packets[i]);
      matched += expected;

      MbufChain whole(packets[i], packets[i].size());
      ASSERT_EQ(expected, program->Matches(&whole.seg[0]))
          << "packet " << i << " (" << packets[i].size()
          << " bytes), whole: " << Hex(packets[i]);

      // A split anywhere, including inside the headers a filter reads.
      if (!packets[i].empty()) {
        const size_t split = 1 + rng() % packets[i].size();
        MbufChain chained(packets[i], split);
        ASSERT_EQ(expected, program->Matches(&chained.seg[0]))
            << "packet " << i << " (" << packets[i].size()
            << " bytes), split at " << split << ": " << Hex(packets[i]);
      }
    }
    // The corpus must exercise both outcomes, or agreement proves little.
    EXPECT_GT(matched, 0u);
    EXPECT_LT(matched, packets.size());
  }
}

TEST(BpfProgramTest, EmptyExpressionMatchesEverything) {
  std::string error;
  auto program = BpfProgram::Compile("", &error);
  ASSERT_NE(nullptr, program) << error;
  std::vector<uint8_t> bytes(64, 0);
  MbufChain m(bytes, bytes.size());
  EXPECT_TRUE(program->Matches(&m.seg[0]));
}

TEST(BpfProgramTest, InvalidExpressionIsRefusedWithAReason) {
  std::string error;
  EXPECT_EQ(nullptr, BpfProgram::Compile("tcp port", &error));
  EXPECT_NE(std::string::npos, error.find("BPF compilation error")) << error;
  EXPECT_EQ(nullptr, BpfProgram::Compile("ip and", &error));
}

#if defined(__x86_64__)
// DPDK's x86-64 JIT handles the packet loads converted classic programs use;
// losing it (a DPDK change) would silently fall back to the interpreter.
TEST(BpfProgramTest, RunsAsNativeCodeOnX86) {
  std::string error;
  auto program = BpfProgram::Compile("tcp port 80", &error);
  ASSERT_NE(nullptr, program) << error;
  EXPECT_TRUE(program->jitted());
}
#endif

}  // namespace
