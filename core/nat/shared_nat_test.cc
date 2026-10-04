// SPDX-License-Identifier: BSD-3-Clause
// SharedNat (TP6, D-079): the NAT every worker translates through.

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "nat/nat.h"
#include "rcu/rcu_domain.h"

namespace bess::nat {
namespace {

using conntrack::ParsedFlowPacket;
using conntrack::ParseFrame;
using conntrack::ParseStatus;

constexpr uint32_t kPublic = 0xc6336401, kRemote = 0x08080808;

std::vector<uint8_t> Udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport) {
  std::vector<uint8_t> f(60, 0);
  f[12] = 0x08;
  f[14] = 0x45, f[17] = 28, f[22] = 64, f[23] = 17;
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(src >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  }
  f[34] = static_cast<uint8_t>(sport >> 8), f[35] = static_cast<uint8_t>(sport);
  f[36] = static_cast<uint8_t>(dport >> 8), f[37] = static_cast<uint8_t>(dport);
  f[39] = 8;
  return f;
}

uint16_t SrcPort(const std::vector<uint8_t> &f) {
  return static_cast<uint16_t>(f[34] << 8 | f[35]);
}

template <typename N>
typename N::Config Config(rcu::RcuDomain *domain, size_t capacity) {
  typename N::Config c;
  c.addresses = {{utils::be32_t(kPublic), {{1024, 65536, false}}}};
  c.capacity = capacity;
  c.granularity_shift = 0;
  c.timeout = 1000;
  c.seed = 11;
  c.rcu = domain;
  return c;
}

TEST(SharedNatTest, NeedsAnRcuDomain) {
  auto made = SharedNat::Create(Config<SharedNat>(nullptr, 64));
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(SharedNat::CreateError::kNoRcuDomain, made.error());
}

// On one thread, a SharedNat answers exactly as a Nat with the same seed:
// verdicts, chosen ports, expiry. (Not refusals when full: an expired shared
// binding holds its slot until a grace period after the next create that finds
// the table full, so the shared table refuses a little earlier.)
TEST(SharedNatTest, OneThreadMatchesTheOwnedNat) {
  rcu::RcuDomain domain(4);
  ASSERT_TRUE(domain.Register(1).has_value());
  domain.Online(1);
  auto owned = Nat::Create(Config<Nat>(nullptr, 1024)).value();
  auto shared = SharedNat::Create(Config<SharedNat>(&domain, 1024)).value();
  uint64_t now = 0;
  size_t expired = 0;
  // EXPECT and break, not ASSERT: the table's destructor waits for a grace
  // period, so the reader must go offline before it runs.
  for (int i = 0; i < 400 && !HasFailure(); i++) {
    const uint32_t host = 0x0a000000u + i % 23;
    const uint16_t port = static_cast<uint16_t>(2000 + i % 7);
    auto a = Udp(host, port, kRemote, 53), b = a;
    ParsedFlowPacket pa, pb;
    EXPECT_EQ(ParseStatus::kOk, ParseFrame(a, pa));
    EXPECT_EQ(ParseStatus::kOk, ParseFrame(b, pb));
    const Verdict va = owned->Translate(a, pa, Direction::kForward, now);
    const Verdict vb = shared->Translate(b, pb, Direction::kForward, now);
    EXPECT_EQ(va, vb) << i;
    EXPECT_EQ(a, b) << i;
    EXPECT_EQ(owned->size(), shared->size()) << i;
    if (i % 37 == 36) {
      now += 700;  // partly: mappings refreshed since the last step stay
      const size_t eo = owned->Expire(now, ~size_t{0});
      expired += eo;
      EXPECT_EQ(eo, shared->Expire(now, ~size_t{0})) << i;
    }
    domain.Quiescent(1);
  }
  EXPECT_GT(expired, 0u);
  domain.Offline(1);
  shared.reset();
  domain.Unregister(1);
}

// Four workers walk the same sequence of new endpoints at once, so most
// creates race another worker's create of the same endpoint, and expire as
// they go. Each internal endpoint ends with one external port, no port serves
// two endpoints, both keys reach the same binding, and a reply never reaches
// another host.
TEST(SharedNatTest, ConcurrentWorkersBindEachEndpointOnce) {
  rcu::RcuDomain domain(8);
  auto config = Config<SharedNat>(&domain, 65536);
  // 2048 ports for about 1000 live mappings: concurrent picks land in the same
  // bitmap words, so a create outside the lock would hand a port out twice.
  config.addresses = {{utils::be32_t(kPublic), {{1024, 3072, false}}}};
  config.timeout = 1000;
  auto nat = SharedNat::Create(config).value();
  constexpr int kWorkers = 4, kRounds = 20000;
  auto endpoint_of = [](int r) {
    return std::pair<uint32_t, uint16_t>(0x0a000000u + r % 251, static_cast<uint16_t>(5000 + r / 251));
  };
  std::atomic<int> started{0};
  std::atomic<uint64_t> translated{0}, inconsistent{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; w++) {
    workers.emplace_back([&, w] {
      const rcu::ReaderId reader = static_cast<rcu::ReaderId>(w + 1);
      if (!domain.Register(reader).has_value()) std::abort();
      domain.Online(reader);
      started++;
      while (started.load() < kWorkers) {
      }
      for (int r = 0; r < kRounds; r++) {
        const auto [host, port] = endpoint_of(r);
        auto f = Udp(host, port, kRemote, 53);
        ParsedFlowPacket p;
        if (ParseFrame(f, p) != ParseStatus::kOk) std::abort();
        if (nat->Translate(f, p, Direction::kForward, r) == Verdict::kTranslated) {
          translated++;
          auto reply = Udp(kRemote, 53, kPublic, SrcPort(f));
          ParsedFlowPacket q;
          if (ParseFrame(reply, q) != ParseStatus::kOk) std::abort();
          if (nat->Translate(reply, q, Direction::kReverse, r) == Verdict::kTranslated) {
            const uint32_t to = static_cast<uint32_t>(reply[30]) << 24 | reply[31] << 16 |
                                reply[32] << 8 | reply[33];
            const uint16_t to_port = static_cast<uint16_t>(reply[36] << 8 | reply[37]);
            // A reply may race an expiry and find nothing; never another host.
            if (to != host || to_port != port) inconsistent++;
          }
        }
        if (r % 16 == w) (void)nat->Expire(r, 64);
        domain.Quiescent(reader);
      }
      domain.Offline(reader);
    });
  }
  for (auto &t : workers) t.join();
  EXPECT_EQ(0u, inconsistent.load());
  EXPECT_GT(translated.load(), static_cast<uint64_t>(kWorkers) * kRounds / 2);
  std::map<uint16_t, Endpoint> owner_of_port;
  size_t live = 0;
  for (int r = 0; r < kRounds; r++) {
    const auto [host, port] = endpoint_of(r);
    const Endpoint in{utils::be32_t(host), utils::be16_t(port), 17};
    const auto *b = nat->Find(in);
    if (b == nullptr) continue;
    live++;
    EXPECT_EQ(in, b->internal);
    const auto [it, fresh] = owner_of_port.emplace(b->external.port.value(), in);
    EXPECT_TRUE(fresh) << "port " << b->external.port.value() << " bound twice";
    EXPECT_EQ(b, nat->Find(b->external)) << "one binding, two keys";
  }
  EXPECT_EQ(live, nat->size());
  EXPECT_GT(live, 0u);
  nat.reset();
  for (int w = 0; w < kWorkers; w++) domain.Unregister(static_cast<rcu::ReaderId>(w + 1));
}

// Growth under traffic (TP6, table_policy.md 4.2): workers create and look up
// while a control thread grows the table 64 -> 4096 in steps. A mapping, once
// made, keeps its external port through every growth (nothing expires here),
// a reply always reaches its host, and at the end every mapping is reachable
// by both keys, with no port shared.
TEST(SharedNatTest, GrowsUnderTrafficWithoutLosingAMapping) {
  rcu::RcuDomain domain(8);
  auto config = Config<SharedNat>(&domain, 64);
  config.max_capacity = 4096;
  config.timeout = ~uint64_t{0} / 4;
  auto nat = SharedNat::Create(config).value();
  constexpr int kWorkers = 3, kFlows = 3000;
  std::atomic<bool> stop{false};
  std::atomic<int> online{0};
  std::atomic<uint64_t> moved_port{0}, wrong_host{0};
  std::vector<std::atomic<uint32_t>> port_of(kFlows);  // 0: no mapping seen yet
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; w++) {
    workers.emplace_back([&, w] {
      const rcu::ReaderId reader = static_cast<rcu::ReaderId>(w + 1);
      if (!domain.Register(reader).has_value()) std::abort();
      domain.Online(reader);
      online++;
      uint32_t r = static_cast<uint32_t>(w) * 7919u;
      while (!stop.load(std::memory_order_relaxed)) {
        const int flow = static_cast<int>(r++ % kFlows);
        auto f = Udp(0x0a000000u + flow / 50, static_cast<uint16_t>(3000 + flow % 50), kRemote, 53);
        ParsedFlowPacket p;
        if (ParseFrame(f, p) != ParseStatus::kOk) std::abort();
        if (nat->Translate(f, p, Direction::kForward, 1) == Verdict::kTranslated) {
          uint32_t expected = 0;
          const uint32_t got = SrcPort(f);
          if (!port_of[flow].compare_exchange_strong(expected, got) && expected != got) {
            moved_port++;
          }
          auto reply = Udp(kRemote, 53, kPublic, static_cast<uint16_t>(got));
          ParsedFlowPacket q;
          if (ParseFrame(reply, q) != ParseStatus::kOk) std::abort();
          if (nat->Translate(reply, q, Direction::kReverse, 1) != Verdict::kTranslated ||
              (static_cast<uint32_t>(reply[30]) << 24 | reply[31] << 16 | reply[32] << 8 |
               reply[33]) != 0x0a000000u + flow / 50) {
            wrong_host++;
          }
        }
        domain.Quiescent(reader);
      }
      domain.Offline(reader);
    });
  }
  while (online.load() < kWorkers) {
  }
  // The control thread (not a reader): grow whenever asked, as the module's
  // request handler does, until the maximum.
  size_t grown = 0;
  while (nat->capacity() < 4096) {
    if (nat->NeedsGrowth()) {
      ASSERT_TRUE(nat->Grow(nat->GrowthTarget()));
      grown++;
    }
    std::this_thread::yield();
  }
  // Let the workers run on the final table for a moment.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop = true;
  for (auto &t : workers) t.join();
  EXPECT_EQ(6u, grown) << "64 -> 4096 by doubling";
  EXPECT_EQ(6u, nat->migrated_tables());
  EXPECT_EQ(0u, moved_port.load()) << "a mapping changed its port";
  EXPECT_EQ(0u, wrong_host.load()) << "a reply missed or reached another host";
  std::set<uint16_t> ports;
  size_t mapped = 0;
  for (int flow = 0; flow < kFlows; flow++) {
    const Endpoint in{utils::be32_t(0x0a000000u + flow / 50), utils::be16_t(3000 + flow % 50), 17};
    const auto *b = nat->Find(in);
    if (port_of[flow].load() != 0) {
      ASSERT_NE(nullptr, b) << "mapping " << flow << " lost";
      EXPECT_EQ(port_of[flow].load(), b->external.port.value());
    }
    if (b == nullptr) continue;
    mapped++;
    EXPECT_TRUE(ports.insert(b->external.port.value()).second);
    EXPECT_EQ(b, nat->Find(b->external));
  }
  EXPECT_EQ(mapped, nat->size());
  EXPECT_EQ(static_cast<size_t>(kFlows), mapped) << "every flow mapped once the table was large enough";
  nat.reset();
  for (int w = 0; w < kWorkers; w++) domain.Unregister(static_cast<rcu::ReaderId>(w + 1));
}

}  // namespace
}  // namespace bess::nat
