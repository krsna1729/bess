// SPDX-License-Identifier: BSD-3-Clause
// Usage counters and records (TP7, D-083): counts per mapping, final records
// when a mapping ends, interim records on request; through growth and on
// every worker.

#include <atomic>
#include <algorithm>
#include <map>
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

std::vector<uint8_t> Udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                         size_t payload = 0) {
  // At least a minimum Ethernet frame (60 bytes, padding after the IP
  // packet): a shorter buffer is smaller than the TCP checksum offset, which
  // GCC 14 at -O3 flags on the rewrite's TCP path (-Wstringop-overflow).
  std::vector<uint8_t> f(std::max<size_t>(14 + 28 + payload, 60), 0);
  f[12] = 0x08;
  f[14] = 0x45, f[22] = 64, f[23] = 17;
  const uint16_t ip_len = static_cast<uint16_t>(28 + payload);
  f[16] = static_cast<uint8_t>(ip_len >> 8), f[17] = static_cast<uint8_t>(ip_len);
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(src >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  }
  f[34] = static_cast<uint8_t>(sport >> 8), f[35] = static_cast<uint8_t>(sport);
  f[36] = static_cast<uint8_t>(dport >> 8), f[37] = static_cast<uint8_t>(dport);
  const uint16_t udp_len = static_cast<uint16_t>(8 + payload);
  f[38] = static_cast<uint8_t>(udp_len >> 8), f[39] = static_cast<uint8_t>(udp_len);
  return f;
}

uint16_t SrcPort(const std::vector<uint8_t> &f) { return static_cast<uint16_t>(f[34] << 8 | f[35]); }

template <typename N>
typename N::Config Config(size_t capacity, size_t log = 64, rcu::RcuDomain *domain = nullptr) {
  typename N::Config c;
  c.addresses = {{utils::be32_t(kPublic), {{1024, 65536, false}}}};
  c.capacity = capacity;
  c.granularity_shift = 0;
  c.timeout = 1000;
  c.seed = 3;
  c.usage_log = log;
  c.rcu = domain;
  return c;
}

template <typename N>
Verdict Send(N &nat, std::vector<uint8_t> &f, Direction dir, uint64_t now) {
  ParsedFlowPacket p;
  EXPECT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  return nat.Translate(f, p, dir, now);
}

std::vector<UsageRecord> Drain(auto &nat) {
  std::vector<UsageRecord> out;
  nat.DrainUsage(&out, ~size_t{0});
  return out;
}

// Both directions count, in IP bytes; a mapping's end is one final record
// with its totals; a full log delays the end (the mapping and its port live
// on) instead of losing the record.
TEST(NatUsageTest, CountsBothDirectionsAndEndsWithAFinalRecord) {
  auto nat = CountedNat::Create(Config<CountedNat>(16, 2)).value();
  uint16_t ext_port = 0;
  for (int i = 0; i < 3; i++) {
    auto out = Udp(0x0a000001, 4000, kRemote, 53, 100);  // 128 IP bytes
    ASSERT_EQ(Verdict::kTranslated, Send(*nat, out, Direction::kForward, 10));
    ext_port = SrcPort(out);
  }
  auto in = Udp(kRemote, 53, kPublic, ext_port, 20);  // 48 IP bytes
  ASSERT_EQ(Verdict::kTranslated, Send(*nat, in, Direction::kReverse, 10));
  // Two more mappings fill the log of 2 when all three expire.
  for (uint16_t port : {4001, 4002}) {
    auto out = Udp(0x0a000001, port, kRemote, 53);
    ASSERT_EQ(Verdict::kTranslated, Send(*nat, out, Direction::kForward, 10));
  }
  EXPECT_EQ(2u, nat->Expire(2000, ~size_t{0})) << "the log holds two final records";
  EXPECT_EQ(1u, nat->size()) << "the third waits for room";
  auto records = Drain(*nat);
  ASSERT_EQ(2u, records.size());
  EXPECT_EQ(1u, nat->Expire(2000 + CountedNat::kLogRetry, ~size_t{0}));
  for (const auto &r : Drain(*nat)) records.push_back(r);
  ASSERT_EQ(3u, records.size());
  std::map<uint16_t, UsageRecord> by_port;
  for (const auto &r : records) {
    EXPECT_TRUE(r.final);
    by_port[r.internal.port.value()] = r;
  }
  EXPECT_EQ(4u, by_port[4000].packets);
  EXPECT_EQ(3u * 128 + 48, by_port[4000].bytes);
  EXPECT_EQ(ext_port, by_port[4000].external.port.value());
  EXPECT_EQ(1u, by_port[4001].packets);
  EXPECT_EQ(28u, by_port[4001].bytes);
  EXPECT_EQ(0u, nat->size());
}

// A requested report gives one interim record per live mapping, with the
// counts so far, over a few batches; the mappings live on.
TEST(NatUsageTest, AReportGivesEveryLiveMappingOnce) {
  auto nat = CountedNat::Create(Config<CountedNat>(256, 512)).value();
  for (uint16_t port = 5000; port < 5100; port++) {
    auto out = Udp(0x0a000002, port, kRemote, 53);
    ASSERT_EQ(Verdict::kTranslated, Send(*nat, out, Direction::kForward, 1));
  }
  nat->RequestReport();
  uint64_t batches = 0;
  while (nat->reports_done() == 0) {
    (void)nat->Expire(1, 16);  // the module's per-batch call
    ASSERT_LT(++batches, 100u);
  }
  EXPECT_GT(batches, 1u) << "spread over batches";
  const auto records = Drain(*nat);
  ASSERT_EQ(100u, records.size());
  std::map<uint16_t, int> seen;
  for (const auto &r : records) {
    EXPECT_FALSE(r.final);
    EXPECT_EQ(1u, r.packets);
    seen[r.internal.port.value()]++;
  }
  EXPECT_EQ(100u, seen.size());
  EXPECT_EQ(100u, nat->size());
}

// Owned growth moves counts with the mapping.
TEST(NatUsageTest, CountsMoveWithAGrowingTable) {
  auto c = Config<CountedGrowableNat>(4, 64);
  c.max_capacity = 16;
  auto nat = CountedGrowableNat::Create(c).value();
  for (uint16_t port = 6000; port < 6003; port++) {
    for (int k = 0; k < 2; k++) {
      auto out = Udp(0x0a000003, port, kRemote, 53);
      ASSERT_EQ(Verdict::kTranslated, Send(*nat, out, Direction::kForward, 1));
    }
  }
  nat->Adopt(CountedGrowableNat::NewTable(nat->GrowthTarget()));
  while (nat->MigrateSome(1) == nullptr) {
  }
  ASSERT_EQ(3u, nat->Expire(5000, ~size_t{0}));
  const auto records = Drain(*nat);
  ASSERT_EQ(3u, records.size());
  for (const auto &r : records) EXPECT_EQ(2u, r.packets) << r.internal.port.value();
}

// A report interrupted by growth starts over in the new table: every live
// mapping is in it at least once (the slots it had walked mean other bindings
// after the move).
TEST(NatUsageTest, AReportInterruptedByGrowthMissesNoMapping) {
  auto c = Config<CountedGrowableNat>(128, 4096);
  c.max_capacity = 256;
  auto nat = CountedGrowableNat::Create(c).value();
  for (uint16_t port = 8000; port < 8096; port++) {  // 96: 3/4 of 128
    auto out = Udp(0x0a000005, port, kRemote, 53);
    ASSERT_EQ(Verdict::kTranslated, Send(*nat, out, Direction::kForward, 1));
  }
  nat->RequestReport();
  (void)nat->Expire(1, 16);  // walks the first 64 slots
  ASSERT_EQ(0u, nat->reports_done());
  nat->Adopt(CountedGrowableNat::NewTable(nat->GrowthTarget()));
  while (nat->MigrateSome(256) == nullptr) {
  }
  while (nat->reports_done() == 0) (void)nat->Expire(1, 16);
  std::map<uint16_t, int> seen;
  for (const auto &r : Drain(*nat)) seen[r.internal.port.value()]++;
  EXPECT_EQ(96u, seen.size()) << "every mapping reported";
}

// Shared: four workers count one mapping set while the control thread grows
// the table; after expiry the final records add up to every packet sent.
TEST(NatUsageTest, SharedCountsAddUpAcrossWorkersAndGrowth) {
  rcu::RcuDomain domain(8);
  auto c = Config<CountedSharedNat>(16, 4096, &domain);
  c.max_capacity = 1024;
  c.timeout = 1000000;  // the workers translate at tick 1: nothing expires during the run
  auto nat = CountedSharedNat::Create(c).value();
  constexpr int kWorkers = 4, kFlows = 300, kRounds = 20;
  std::atomic<int> online{0}, finished{0};
  std::atomic<uint64_t> sent{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; w++) {
    workers.emplace_back([&, w] {
      const rcu::ReaderId reader = static_cast<rcu::ReaderId>(w + 1);
      if (!domain.Register(reader).has_value()) std::abort();
      domain.Online(reader);
      online++;
      for (int round = 0; round < kRounds; round++) {
        for (int flow = 0; flow < kFlows; flow++) {
          // A packet the full table refuses is sent again once it has grown
          // (as a flow's next packet would be): every flow ends up mapped
          // whatever the scheduling, with growth under way meanwhile.
          Verdict v;
          do {
            auto out = Udp(0x0a000004, static_cast<uint16_t>(7000 + flow), kRemote, 53);
            ParsedFlowPacket p;
            if (ParseFrame(out, p) != ParseStatus::kOk) std::abort();
            v = nat->Translate(out, p, Direction::kForward, 1);
            domain.Quiescent(reader);
            if (v == Verdict::kFull) std::this_thread::yield();
          } while (v == Verdict::kFull);
          if (v == Verdict::kTranslated) sent++;
        }
      }
      domain.Offline(reader);
      finished++;
    });
  }
  while (online.load() < kWorkers) {
    std::this_thread::yield();
  }
  // Grow whenever asked (as the module's handler does) until the workers are
  // done and nothing more is asked.
  size_t grown = 0;
  while (finished.load() < kWorkers || nat->NeedsGrowth()) {
    if (nat->NeedsGrowth()) {
      if (!nat->Grow(nat->GrowthTarget())) {
        ADD_FAILURE() << "growth failed";
        break;
      }
      grown++;
    }
    std::this_thread::yield();
  }
  for (auto &t : workers) t.join();
  EXPECT_GE(grown, 5u) << "16 -> 512 for 300 flows";
  EXPECT_EQ(static_cast<size_t>(kFlows), nat->size());
  EXPECT_EQ(static_cast<uint64_t>(kWorkers) * kRounds * kFlows, sent.load());
  // End everything: one final record per mapping.
  ASSERT_TRUE(domain.Register(6).has_value());
  domain.Online(6);
  // EXPECT, not ASSERT: returning with reader 6 online would hang the NAT's
  // destructor (it waits for a grace period).
  EXPECT_EQ(static_cast<size_t>(kFlows), nat->Expire(2000000, ~size_t{0}));
  domain.Offline(6);
  domain.Unregister(6);
  uint64_t total = 0;
  for (const auto &r : Drain(*nat)) total += r.packets;
  EXPECT_EQ(sent.load(), total);
  nat.reset();
  for (int w = 0; w < kWorkers; w++) domain.Unregister(static_cast<rcu::ReaderId>(w + 1));
}

}  // namespace
}  // namespace bess::nat
