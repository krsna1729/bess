// SPDX-License-Identifier: BSD-3-Clause

// SharedConntrack (TP8, D-081): the same verdicts as Conntrack, for every
// worker at once.

#include "conntrack/shared_conntrack.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "conntrack/conntrack.h"
#include "rcu/rcu_domain.h"

namespace bess::conntrack {
namespace {

struct Ep {
  uint32_t ip;
  uint16_t port;
};

void Put16(std::vector<uint8_t> &f, size_t off, uint16_t v) {
  f[off] = static_cast<uint8_t>(v >> 8);
  f[off + 1] = static_cast<uint8_t>(v);
}

std::vector<uint8_t> Frame(Ep s, Ep d, uint8_t proto, uint8_t flags_or_type) {
  std::vector<uint8_t> f(14 + 20, 0);
  f[12] = 0x08;
  f[14] = 0x45;
  f[22] = 64;
  f[23] = proto;
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(s.ip >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(d.ip >> (24 - 8 * i));
  }
  std::vector<uint8_t> l4(proto == 6 ? 20 : 8, 0);
  if (proto == 6 || proto == 17) {
    Put16(l4, 0, s.port);
    Put16(l4, 2, d.port);
  }
  if (proto == 6) {
    l4[12] = 5 << 4;
    l4[13] = flags_or_type;
  } else if (proto == 17) {
    Put16(l4, 4, 8);
  } else {
    l4[0] = flags_or_type;  // ICMP type
    Put16(l4, 4, s.port);   // identifier
  }
  f.insert(f.end(), l4.begin(), l4.end());
  Put16(f, 16, static_cast<uint16_t>(f.size() - 14));
  return f;
}

using Owned = Conntrack<>;
using Shared = SharedConntrack<>;

// One thread, a random stream over a few hosts: every packet gets the same
// status and direction from both trackers, and expiry removes the same number
// of connections at the same times.
TEST(SharedConntrackTest, OneThreadMatchesTheOwnedTracker) {
  rcu::RcuDomain domain(2);
  ASSERT_TRUE(domain.Register(1).has_value());
  domain.Online(1);
  TimeoutPolicy policy;
  policy.tcp_pickup = true;
  auto owned = Owned::Create(4096, policy).value();
  auto shared = Shared::Create(4096, domain, policy).value();
  std::mt19937 rng(7);
  const uint8_t flags[] = {kTcpSyn, kTcpSyn | kTcpAck, kTcpAck, kTcpFin | kTcpAck, kTcpRst,
                           kTcpFin, kTcpSyn | kTcpFin};
  uint64_t now = 0;
  size_t compared = 0, removed = 0;
  for (int i = 0; i < 20000 && !HasFailure(); i++) {
    const Ep a{static_cast<uint32_t>(0x0a000001u + rng() % 4), static_cast<uint16_t>(1000 + rng() % 4)};
    const Ep b{static_cast<uint32_t>(0x08080800u + rng() % 3), static_cast<uint16_t>(rng() % 2 ? 80 : 53)};
    const bool forward = rng() % 2;
    const uint8_t proto = rng() % 3 == 0 ? 17 : rng() % 5 == 0 ? 1 : 6;
    const uint8_t fl = proto == 1 ? (rng() % 2 ? 8 : 0) : flags[rng() % 7];
    const auto f = forward ? Frame(a, b, proto, fl) : Frame(b, a, proto, fl);
    ParsedFlowPacket p;
    if (ParseFrame(f, p) != ParseStatus::kOk) continue;
    const bool may_create = rng() % 4 != 0;
    const auto o = owned->Track(f, p, now, 0, may_create);
    const auto s = shared->Track(f, p, now, 0, may_create);
    EXPECT_EQ(o.status, s.status) << i;
    if (o.status != TrackStatus::kInvalid) {
      EXPECT_EQ(o.direction, s.direction) << i;
    }
    EXPECT_EQ(owned->size(), shared->size()) << i;
    compared++;
    if (rng() % 50 == 0) {
      now += rng() % 200;
      const size_t ro = owned->Expire(now, ~size_t{0});
      EXPECT_EQ(ro, shared->Expire(now, ~size_t{0})) << i;
      removed += ro;
    }
    domain.Quiescent(1);
  }
  EXPECT_GT(compared, 10000u);
  EXPECT_GT(removed, 0u);
  domain.Offline(1);
  shared.reset();
  domain.Unregister(1);
}

// Four workers send the SYN of the same new connections at once: each
// connection is created exactly once (one kNew, the others see it), and the
// table holds one entry per connection.
TEST(SharedConntrackTest, ConcurrentSynsCreateEachConnectionOnce) {
  rcu::RcuDomain domain(8);
  auto ct = Shared::Create(8192, domain).value();
  constexpr int kWorkers = 4, kConns = 4000;
  std::vector<std::atomic<int>> created(kConns);
  std::atomic<int> started{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; w++) {
    workers.emplace_back([&, w] {
      const rcu::ReaderId reader = static_cast<rcu::ReaderId>(w + 1);
      if (!domain.Register(reader).has_value()) std::abort();
      domain.Online(reader);
      started++;
      while (started.load() < kWorkers) {
      }
      for (int c = 0; c < kConns; c++) {
        const Ep client{0x0a000000u + c / 100, static_cast<uint16_t>(2000 + c % 100)};
        const auto f = Frame(client, Ep{0x08080808, 80}, 6, kTcpSyn);
        ParsedFlowPacket p;
        if (ParseFrame(f, p) != ParseStatus::kOk) std::abort();
        if (ct->Track(f, p, 1).status == TrackStatus::kNew) created[c]++;
        if (c % 32 == w) (void)ct->Expire(1, 16);
        domain.Quiescent(reader);
      }
      domain.Offline(reader);
    });
  }
  for (auto &t : workers) t.join();
  for (int c = 0; c < kConns; c++) ASSERT_EQ(1, created[c].load()) << c;
  EXPECT_EQ(static_cast<size_t>(kConns), ct->size());
  ct.reset();
  for (int w = 0; w < kWorkers; w++) domain.Unregister(static_cast<rcu::ReaderId>(w + 1));
}

// The two directions of established connections on different workers, with
// expiry running: replies are tracked as replies (never invalid) while the
// connection lives, and no packet of a live connection is lost to a race.
TEST(SharedConntrackTest, DirectionsOnDifferentWorkersStayTracked) {
  rcu::RcuDomain domain(8);
  TimeoutPolicy policy;  // established: 5 days; nothing expires here
  auto ct = Shared::Create(4096, domain, policy).value();
  constexpr int kConns = 500;
  auto client = [](int c) { return Ep{0x0a000000u + c / 50, static_cast<uint16_t>(3000 + c % 50)}; };
  const Ep server{0x08080808, 443};
  {
    ASSERT_TRUE(domain.Register(7).has_value());
    domain.Online(7);
    for (int c = 0; c < kConns; c++) {
      for (const auto &[from_client, fl] : {std::pair{true, kTcpSyn}, std::pair{false, uint8_t(kTcpSyn | kTcpAck)},
                                            std::pair{true, kTcpAck}}) {
        const auto f = from_client ? Frame(client(c), server, 6, fl) : Frame(server, client(c), 6, fl);
        ParsedFlowPacket p;
        ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p));
        ASSERT_NE(TrackStatus::kInvalid, ct->Track(f, p, 1).status) << c;
      }
    }
    domain.Offline(7);
    domain.Unregister(7);
  }
  std::atomic<int> started{0};
  std::atomic<uint64_t> bad{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < 4; w++) {
    workers.emplace_back([&, w] {
      const rcu::ReaderId reader = static_cast<rcu::ReaderId>(w + 1);
      if (!domain.Register(reader).has_value()) std::abort();
      domain.Online(reader);
      started++;
      while (started.load() < 4) {
      }
      const bool from_client = w % 2 == 0;  // workers 0, 2 originals; 1, 3 replies
      for (int round = 0; round < 20; round++) {
        for (int c = 0; c < kConns; c++) {
          const auto f = from_client ? Frame(client(c), server, 6, kTcpAck)
                                     : Frame(server, client(c), 6, kTcpAck);
          ParsedFlowPacket p;
          if (ParseFrame(f, p) != ParseStatus::kOk) std::abort();
          const auto r = ct->Track(f, p, 2 + round, 0, false);
          if (r.status != TrackStatus::kExisting ||
              r.direction != (from_client ? Direction::kOriginal : Direction::kReply)) {
            bad++;
          }
          if (c % 64 == w) (void)ct->Expire(2 + round, 32);
          domain.Quiescent(reader);
        }
      }
      domain.Offline(reader);
    });
  }
  for (auto &t : workers) t.join();
  EXPECT_EQ(0u, bad.load());
  EXPECT_EQ(static_cast<size_t>(kConns), ct->size());
  ct.reset();
  for (int w = 0; w < 4; w++) domain.Unregister(static_cast<rcu::ReaderId>(w + 1));
}

}  // namespace
}  // namespace bess::conntrack
