// SPDX-License-Identifier: BSD-3-Clause

// Three appliance-shaped users of the flow tables, each written only against
// the public API of flow/ -- no table of their own. They are the roadmap's M9
// exit check that the state of different appliances is expressible without
// another core table implementation, and they share nothing with each other
// except the library: a NAT binding with a reverse key and idle expiry, an L2
// forwarding table learned from source addresses and aged by scan, and a
// load balancer's connection table shared by several workers.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "flow/shared_flow_table.h"
#include "flow/worker_flow_table.h"
#include "rcu/rcu_domain.h"

namespace bess::flow {
namespace {

// -- 1. NAT bindings: worker-owned, forward + reverse key, idle expiry -----------

struct Tuple {
  uint32_t src, dst;
  uint16_t sport, dport;
  uint8_t proto;
  uint8_t pad[3];
};
static_assert(ByteHashableFlowKey<Tuple>);

struct Binding {
  Binding(uint32_t ext_ip, uint16_t ext_port, uint64_t now)
      : ext_ip(ext_ip), ext_port(ext_port), last_seen(now) {}
  uint32_t ext_ip;
  uint16_t ext_port;
  uint64_t last_seen;  // the timer substrate would own this; a plain store
  uint64_t packets = 0;
};

// Remembers when each binding was created; expiry looks at last_seen.
struct NatObserver {
  std::vector<FlowHandle> live;
  void OnCreate(FlowHandle h, Binding &) noexcept { live.push_back(h); }
  void OnErase(FlowHandle h, Binding &) noexcept {
    live.erase(std::find(live.begin(), live.end(), h));
  }
  void OnFull() noexcept {}
};
struct NatTraits : DefaultFlowTableTraits {
  static constexpr size_t kAliases = 1;
  using Observer = NatObserver;
};
using NatTable = WorkerFlowTable<Tuple, Binding, DefaultFlowHash<Tuple>,
                                 DefaultFlowEqual<Tuple>, NatTraits>;

class Nat {
 public:
  explicit Nat(size_t capacity)
      : table_(std::move(*NatTable::Create(capacity))) {}

  // Translates an inside-to-outside packet: finds or creates the binding.
  // Returns false (drop) if the table is full.
  bool Outbound(Tuple &packet, uint64_t now) {
    FlowRef<Binding> ref = table_->FindRef(packet);
    Binding *b = ref.state;
    if (b == nullptr) {
      // The reverse key: the server's reply, addressed to the external side.
      const uint16_t port = next_port_++;
      Tuple reverse{packet.dst, kExternalIp, packet.dport, port, packet.proto, {}};
      auto made = table_->EmplaceAliased(packet, reverse, kExternalIp, port, now);
      if (!made) return false;
      b = made.state;
    }
    b->last_seen = now;
    b->packets++;
    packet.src = b->ext_ip;
    packet.sport = b->ext_port;
    return true;
  }

  // Translates an outside-to-inside packet; false if no binding (drop).
  bool Inbound(Tuple &packet, uint64_t now, const Tuple &original_of_binding) {
    Binding *b = table_->Find(packet);
    if (b == nullptr) return false;
    b->last_seen = now;
    b->packets++;
    packet.dst = original_of_binding.src;
    packet.dport = original_of_binding.sport;
    return true;
  }

  size_t ExpireIdle(uint64_t now, uint64_t timeout) {
    std::vector<FlowHandle> expired;
    for (FlowHandle h : table_->observer().live) {
      if (now - table_->Lookup(h)->last_seen >= timeout) expired.push_back(h);
    }
    for (FlowHandle h : expired) table_->Erase(h);
    return expired.size();
  }

  NatTable &table() { return *table_; }
  static constexpr uint32_t kExternalIp = 0xc0a80001;

 private:
  std::unique_ptr<NatTable> table_;
  uint16_t next_port_ = 1024;
};

TEST(ReferenceAppsTest, NatBindingsTranslateBothWaysAndExpire) {
  Nat nat(64);
  const Tuple client{0x0a000002, 0x08080808, 5000, 53, 17, {}};

  Tuple out = client;
  ASSERT_TRUE(nat.Outbound(out, 100));
  EXPECT_EQ(Nat::kExternalIp, out.src);
  const uint16_t ext_port = out.sport;
  EXPECT_GE(ext_port, 1024);

  // The same flow again reuses the binding; the reply, keyed by the reverse
  // tuple, finds it through the alias and is translated back.
  Tuple again = client;
  ASSERT_TRUE(nat.Outbound(again, 110));
  EXPECT_EQ(ext_port, again.sport);
  EXPECT_EQ(1u, nat.table().size());
  Tuple reply{client.dst, Nat::kExternalIp, client.dport, ext_port, 17, {}};
  ASSERT_TRUE(nat.Inbound(reply, 120, client));
  EXPECT_EQ(client.src, reply.dst);
  EXPECT_EQ(client.sport, reply.dport);
  EXPECT_EQ(3u, nat.table().Find(client)->packets);

  // A reply to a port nobody holds is dropped.
  Tuple stray{client.dst, Nat::kExternalIp, client.dport, 4242, 17, {}};
  EXPECT_FALSE(nat.Inbound(stray, 125, client));

  // Many clients, then the idle ones expire; both keys of each go away.
  std::vector<Tuple> clients;
  for (uint16_t i = 0; i < 40; i++) {
    clients.push_back({0x0a000100u + i, 0x08080808, static_cast<uint16_t>(6000 + i), 443, 6, {}});
    Tuple p = clients.back();
    ASSERT_TRUE(nat.Outbound(p, 200 + i));
  }
  EXPECT_EQ(41u, nat.table().size());
  EXPECT_EQ(22u, nat.ExpireIdle(/*now=*/250, /*timeout=*/30))
      << "the first client and the 21 oldest of the 40";
  EXPECT_EQ(19u, nat.table().size());
  EXPECT_EQ(nullptr, nat.table().Find(client));
  EXPECT_EQ(nullptr, nat.table().Find(reply)) << "the reverse key outlived the binding";
  EXPECT_FALSE(nat.Inbound(reply, 251, client));
  EXPECT_NE(nullptr, nat.table().Find(clients.back()));
}

TEST(ReferenceAppsTest, NatDropsNewSessionsWhenFullAndRecoversOnExpiry) {
  Nat nat(8);
  for (uint16_t i = 0; i < 8; i++) {
    Tuple p{0x0a000200u + i, 0x01010101, 7000, 80, 6, {}};
    ASSERT_TRUE(nat.Outbound(p, 10));
  }
  Tuple extra{0x0a0002ff, 0x01010101, 7000, 80, 6, {}};
  EXPECT_FALSE(nat.Outbound(extra, 11)) << "a full table drops the new session";
  EXPECT_EQ(8u, nat.table().size());
  EXPECT_EQ(8u, nat.ExpireIdle(100, 50));
  EXPECT_TRUE(nat.Outbound(extra, 101));
}

// -- 2. L2 forwarding table: worker-owned, learned, aged by scan -----------------

struct FdbKey {
  uint8_t mac[6];
  uint16_t vlan;
};
static_assert(ByteHashableFlowKey<FdbKey>);

struct FdbEntry {
  FdbEntry(uint16_t port, uint64_t now) : port(port), learned_at(now) {}
  uint16_t port;
  uint64_t learned_at;
};
using Fdb = WorkerFlowTable<FdbKey, FdbEntry>;

FdbKey Mac(uint8_t last, uint16_t vlan = 1) {
  return FdbKey{{0x02, 0, 0, 0, 0, last}, vlan};
}

TEST(ReferenceAppsTest, L2TableLearnsMovesAndAges) {
  auto fdb = std::move(*Fdb::Create(4));
  constexpr uint16_t kFlood = 0xffff;
  auto forward = [&](const FdbKey &dst) {
    const FdbEntry *e = fdb->Find(dst);
    return e != nullptr ? e->port : kFlood;
  };
  auto learn = [&](const FdbKey &src, uint16_t port, uint64_t now) {
    auto r = fdb->Emplace(src, port, now);
    if (r.status == EmplaceStatus::kExists) {  // seen again, possibly moved
      r.state->port = port;
      r.state->learned_at = now;
    }
    return static_cast<bool>(r);
  };

  EXPECT_EQ(kFlood, forward(Mac(1)));
  EXPECT_TRUE(learn(Mac(1), 3, 10));
  EXPECT_EQ(3u, forward(Mac(1)));
  EXPECT_EQ(kFlood, forward(Mac(1, /*vlan=*/2))) << "the VLAN is part of the key";
  EXPECT_TRUE(learn(Mac(1), 7, 20));  // the host moved
  EXPECT_EQ(7u, forward(Mac(1)));
  EXPECT_EQ(1u, fdb->size());

  // The table is small: a fifth address is not learned, and traffic to it
  // floods, which is the bridge's behaviour for an unknown destination.
  for (uint8_t m = 2; m <= 4; m++) ASSERT_TRUE(learn(Mac(m), m, 30));
  EXPECT_FALSE(learn(Mac(5), 5, 31));
  EXPECT_EQ(kFlood, forward(Mac(5)));

  // Aging by periodic scan.
  std::vector<FlowHandle> stale;
  fdb->ForEach([&](FlowHandle h, const FdbKey &, FdbEntry &e) {
    if (40 - e.learned_at >= 15) stale.push_back(h);
  });
  EXPECT_EQ(1u, stale.size()) << "only the entry learned at 20";
  for (FlowHandle h : stale) EXPECT_TRUE(fdb->Erase(h));
  EXPECT_EQ(kFlood, forward(Mac(1)));
  EXPECT_TRUE(learn(Mac(5), 5, 41)) << "the aged slot is available again";
  EXPECT_EQ(5u, forward(Mac(5)));
}

// -- 3. Load balancer connections: shared by workers -----------------------------

struct Connection {
  explicit Connection(uint32_t backend) : backend(backend) {}
  uint32_t backend;
  std::atomic<uint64_t> packets{0};  // shared state brings its own atomics
};
using ConnTable = SharedFlowTable<Tuple, Connection>;

TEST(ReferenceAppsTest, LoadBalancerPinsEachConnectionAcrossWorkers) {
  rcu::RcuDomain domain(16);
  auto table = std::move(*ConnTable::Create(4096, domain));
  constexpr int kWorkers = 4;
  constexpr uint32_t kConnections = 1000;
  constexpr uint32_t kBackends = 8;

  std::vector<std::vector<uint32_t>> seen(kWorkers, std::vector<uint32_t>(kConnections, ~0u));
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; w++) {
    const auto reader = static_cast<rcu::ReaderId>(w);
    ASSERT_TRUE(domain.Register(reader).has_value());
    workers.emplace_back([&, w, reader] {
      domain.Online(reader);
      std::mt19937 rng(w);
      std::vector<uint32_t> order(kConnections);
      for (uint32_t i = 0; i < kConnections; i++) order[i] = i;
      for (int round = 0; round < 5; round++) {
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t c : order) {
          const Tuple key{0x0a000000u + c, 0xc0a80101, static_cast<uint16_t>(10000 + c), 443, 6, {}};
          Connection *conn = table->Find(key);
          if (conn == nullptr) {
            // First packet seen by this worker: whoever creates it first picks
            // the backend; everyone else gets that pick back.
            const auto made = table->Emplace(key, (c * 7 + static_cast<uint32_t>(w)) % kBackends);
            conn = made.state;
          }
          conn->packets.fetch_add(1, std::memory_order_relaxed);
          seen[static_cast<size_t>(w)][c] = conn->backend;
        }
        domain.Quiescent(reader);
      }
      domain.Offline(reader);
    });
  }
  for (auto &t : workers) t.join();
  for (int w = 0; w < kWorkers; w++) domain.Unregister(static_cast<rcu::ReaderId>(w));

  EXPECT_EQ(kConnections, table->size());
  uint64_t packets = 0;
  for (uint32_t c = 0; c < kConnections; c++) {
    for (int w = 1; w < kWorkers; w++) {
      ASSERT_EQ(seen[0][c], seen[static_cast<size_t>(w)][c]) << "connection " << c << " saw two backends";
    }
  }
  table->ForEach([&](FlowHandle, const Tuple &, const Connection &c) { packets += c.packets.load(); });
  EXPECT_EQ(uint64_t{kWorkers} * 5 * kConnections, packets);

  // A backend is removed: its connections are erased, the others are untouched.
  const uint32_t removed = 3;
  std::vector<FlowHandle> doomed;
  size_t kept = 0;
  table->ForEach([&](FlowHandle h, const Tuple &, const Connection &c) {
    if (c.backend == removed) {
      doomed.push_back(h);
    } else {
      kept++;
    }
  });
  for (FlowHandle h : doomed) EXPECT_TRUE(table->Erase(h));
  EXPECT_EQ(kept, table->size());
  for (FlowHandle h : doomed) EXPECT_EQ(nullptr, table->Peek(h));
  table->Reclaim();
  EXPECT_EQ(0u, table->pending_reclaim());
}

}  // namespace
}  // namespace bess::flow
