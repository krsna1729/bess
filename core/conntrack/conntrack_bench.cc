// SPDX-License-Identifier: BSD-3-Clause

// M17 (D-067): connection tracking costs.
//
//   BM_Parse/<kind>          ns per ParseFrame: 0 TCP/IPv4, 1 UDP/IPv4 behind
//                            two VLAN tags, 2 UDP/IPv6 with two extension
//                            headers, 3 a truncated frame (rejected)
//   BM_Track/<case>/<conns>  ns per Track over `conns` established connections
//                            (a parsed packet stream in random connection
//                            order, read sequentially):
//                            0 TCP ACK, original direction (lookup, no
//                            transition); 1 the same in the reply direction;
//                            2 UDP refresh; 3 a full TCP handshake + close
//                            (7 packets: SYN, SYN-ACK, ACK, FIN, ACK, FIN,
//                            ACK; ns per packet; connections recycled)
//   BM_Expire/<conns>        ns per expired connection, all at once

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

#include "conntrack/conntrack.h"

namespace {

using namespace bess::conntrack;

void Put16(std::vector<uint8_t> &f, size_t off, uint16_t v) {
  f[off] = static_cast<uint8_t>(v >> 8);
  f[off + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t> &f, size_t off, uint32_t v) {
  Put16(f, off, static_cast<uint16_t>(v >> 16));
  Put16(f, off + 2, static_cast<uint16_t>(v));
}

std::vector<uint8_t> V4(uint32_t s, uint16_t sp, uint32_t d, uint16_t dp, uint8_t proto,
                        uint8_t flags, int vlans = 0) {
  const size_t ip = 14 + 4 * static_cast<size_t>(vlans);
  const size_t l4 = proto == 6 ? 20 : 12;
  std::vector<uint8_t> f(ip + 20 + l4, 0);
  for (int v = 0; v < vlans; v++) {
    Put16(f, 12 + 4 * v, 0x8100);
    Put16(f, 14 + 4 * v, 1);
  }
  Put16(f, ip - 2, 0x0800);
  f[ip] = 0x45;
  Put16(f, ip + 2, static_cast<uint16_t>(20 + l4));
  f[ip + 8] = 64;
  f[ip + 9] = proto;
  Put32(f, ip + 12, s);
  Put32(f, ip + 16, d);
  Put16(f, ip + 20, sp);
  Put16(f, ip + 22, dp);
  if (proto == 6) {
    f[ip + 32] = 5 << 4;
    f[ip + 33] = flags;
  } else {
    Put16(f, ip + 24, static_cast<uint16_t>(l4));
  }
  return f;
}

std::vector<uint8_t> V6Ext() {
  static constexpr uint8_t kTail[] = {60, 0, 0, 0, 0, 0, 0, 0,          // hop-by-hop -> dest opts
                                      17, 0, 0, 0, 0, 0, 0, 0,          // dest opts -> UDP
                                      0x13, 0x88, 0, 53, 0, 12, 0, 0,   // UDP 5000 -> 53
                                      1, 2, 3, 4};
  const size_t ip = 14;
  std::vector<uint8_t> f(ip + 40 + sizeof(kTail), 0);
  Put16(f, 12, 0x86dd);
  f[ip] = 0x60;
  f[ip + 6] = 0;  // hop-by-hop
  f[ip + 23] = 1;
  f[ip + 39] = 2;
  std::memcpy(f.data() + ip + 40, kTail, sizeof(kTail));
  Put16(f, ip + 4, static_cast<uint16_t>(sizeof(kTail)));
  return f;
}

void BM_Parse(benchmark::State &state) {
  std::vector<uint8_t> f;
  switch (state.range(0)) {
    case 0: f = V4(1, 1000, 2, 80, 6, kTcpAck); break;
    case 1: f = V4(1, 1000, 2, 53, 17, 0, 2); break;
    case 2: f = V6Ext(); break;
    default: f = V4(1, 1000, 2, 80, 6, kTcpAck); f.resize(40); break;
  }
  ParsedFlowPacket p;
  for (auto _ : state) {
    benchmark::DoNotOptimize(ParseFrame(f, p));
    benchmark::DoNotOptimize(p);
  }
}

struct Stream {
  std::vector<std::vector<uint8_t>> frames;
  std::vector<ParsedFlowPacket> parsed;
};

void BM_Track(benchmark::State &state) {
  const int kase = static_cast<int>(state.range(0));
  const auto conns = static_cast<uint32_t>(state.range(1));
  TimeoutPolicy policy;
  auto ct = Conntrack<>::Create(conns * 2 + 64, policy).value();
  auto client = [](uint32_t i) { return 0x0a000000u + i; };
  constexpr uint32_t kServer = 0xc0a80001;
  uint64_t now = 1;
  std::mt19937 rng(0x17);
  Stream s;
  auto add = [&](std::vector<uint8_t> f) {
    ParsedFlowPacket p;
    (void)ParseFrame(f, p);
    s.frames.push_back(std::move(f));
    s.parsed.push_back(p);
  };
  if (kase <= 2) {
    // Establish every connection, then stream one packet kind in random order.
    for (uint32_t i = 0; i < conns; i++) {
      const uint16_t port = static_cast<uint16_t>(1024 + i % 60000);
      if (kase == 2) {
        add(V4(client(i), port, kServer, 53, 17, 0));
      } else {
        for (auto [s1, s2, from_client] :
             {std::tuple{kTcpSyn, 0, true}, {kTcpSyn | kTcpAck, 0, false}, {kTcpAck, 0, true}}) {
          (void)s2;
          auto f = from_client ? V4(client(i), port, kServer, 80, 6, s1)
                               : V4(kServer, 80, client(i), port, 6, s1);
          ParsedFlowPacket p;
          (void)ParseFrame(f, p);
          (void)ct->Track(f, p, now);
        }
        add(kase == 0 ? V4(client(i), port, kServer, 80, 6, kTcpAck)
                      : V4(kServer, 80, client(i), port, 6, kTcpAck));
      }
    }
    // The stream holds the packets in a random connection order and is read
    // sequentially: only the table and the wheel are accessed at random, as
    // on a packet path where the parsed descriptor is the packet's own.
    std::vector<uint32_t> order(conns);
    for (auto &o : order) o = rng() % conns;
    Stream shuffled;
    for (const uint32_t i : order) {
      shuffled.frames.push_back(s.frames[i]);
      shuffled.parsed.push_back(s.parsed[i]);
    }
    s = std::move(shuffled);
    size_t pos = 0;
    uint64_t existing = 0;
    for (auto _ : state) {
      const uint32_t i = static_cast<uint32_t>(pos);
      const auto r = ct->Track(s.frames[i], s.parsed[i], now);
      existing += r.status == TrackStatus::kExisting || r.status == TrackStatus::kNew;
      pos = pos + 1 == conns ? 0 : pos + 1;
    }
    state.counters["accepted_pct"] = 100.0 * existing / state.iterations();
  } else {
    // A handshake and close per connection; connections cycle through ports.
    const uint8_t flags[7] = {kTcpSyn, kTcpSyn | kTcpAck, kTcpAck, kTcpFin | kTcpAck,
                              kTcpAck, kTcpFin | kTcpAck, kTcpAck};
    const bool from_client[7] = {true, false, true, true, false, false, true};
    for (uint32_t i = 0; i < conns; i++) {
      const uint16_t port = static_cast<uint16_t>(1024 + i % 60000);
      for (int k = 0; k < 7; k++) {
        add(from_client[k] ? V4(client(i), port, kServer, 80, 6, flags[k])
                           : V4(kServer, 80, client(i), port, 6, flags[k]));
      }
    }
    size_t pos = 0;
    uint64_t accepted = 0;
    for (auto _ : state) {
      const auto r = ct->Track(s.frames[pos], s.parsed[pos], now);
      accepted += r.status == TrackStatus::kNew || r.status == TrackStatus::kExisting;
      if (++pos == s.frames.size()) {
        pos = 0;
        state.PauseTiming();
        now += 1000;  // past TIME_WAIT: the next round starts fresh
        (void)ct->Expire(now, ~size_t{0});
        state.ResumeTiming();
      }
    }
    state.counters["accepted_pct"] = 100.0 * accepted / state.iterations();
  }
  state.counters["ns_per_packet"] = benchmark::Counter(
      static_cast<double>(state.iterations()),
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

void BM_Expire(benchmark::State &state) {
  const auto conns = static_cast<uint32_t>(state.range(0));
  std::unique_ptr<Conntrack<>> ct;
  for (auto _ : state) {
    state.PauseTiming();
    ct.reset();  // the previous table's teardown is not timed
    ct = Conntrack<>::Create(conns + 64).value();
    for (uint32_t i = 0; i < conns; i++) {
      auto f = V4(0x0a000000u + i, static_cast<uint16_t>(1024 + i % 60000), 0xc0a80001, 53, 17, 0);
      ParsedFlowPacket p;
      (void)ParseFrame(f, p);
      (void)ct->Track(f, p, i % 7);
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(ct->Expire(1u << 20, ~size_t{0}));
  }
  state.counters["ns_per_conn"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * conns,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

BENCHMARK(BM_Parse)->DenseRange(0, 3)->MinTime(0.2);
BENCHMARK(BM_Track)->ArgsProduct({{0, 1, 2}, {1024, 65536, 1048576}})->MinTime(0.3);
BENCHMARK(BM_Track)->Args({3, 1024})->Args({3, 65536})->MinTime(0.3);
BENCHMARK(BM_Expire)->Arg(65536)->Iterations(5);

}  // namespace
