// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/scope_cell.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

using bess::dataplane::ScopeCell;
using bess::dataplane::ScopeId;
using bess::dataplane::ScopePlan;
using bess::dataplane::ScopeTable;
using bess::meter::MeterId;
using bess::route::NextHopId;

TEST(ScopeCellTest, PackingAndUnpacking) {
  // Test zero
  ScopePlan zero{};
  EXPECT_EQ(0u, ScopePlan::Pack(zero));
  ScopePlan unpacked_zero = ScopePlan::Unpack(0);
  EXPECT_EQ(0u, unpacked_zero.meter.value());
  EXPECT_EQ(0u, unpacked_zero.next_hop.value());

  // Test boundaries
  ScopePlan max_plan{
      .meter = MeterId(0xFFFFFFFF),
      .next_hop = NextHopId(0xFFFFFFFF),
  };
  uint64_t max_packed = ScopePlan::Pack(max_plan);
  EXPECT_EQ(0xFFFFFFFFFFFFFFFFULL, max_packed);
  ScopePlan unpacked_max = ScopePlan::Unpack(max_packed);
  EXPECT_EQ(0xFFFFFFFFu, unpacked_max.meter.value());
  EXPECT_EQ(0xFFFFFFFFu, unpacked_max.next_hop.value());

  // Test arbitrary values
  ScopePlan arbitrary{
      .meter = MeterId(12345678),
      .next_hop = NextHopId(87654321),
  };
  uint64_t packed = ScopePlan::Pack(arbitrary);
  ScopePlan unpacked = ScopePlan::Unpack(packed);
  EXPECT_EQ(12345678u, unpacked.meter.value());
  EXPECT_EQ(87654321u, unpacked.next_hop.value());
}

TEST(ScopeCellTest, ConcurrentAtomicReadWriteNeverTears) {
  ScopeCell cell;
  std::atomic<bool> stop{false};

  // Plan A: {100, 100}
  // Plan B: {200, 200}
  // A reader must ALWAYS observe {100, 100} or {200, 200}, NEVER torn {100, 200} or {200, 100}!
  ScopePlan plan_a{.meter = MeterId(100), .next_hop = NextHopId(100)};
  ScopePlan plan_b{.meter = MeterId(200), .next_hop = NextHopId(200)};

  std::thread writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      cell.Store(plan_a);
      cell.Store(plan_b);
    }
  });

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      ScopePlan read = cell.Load();
      if (read.meter.value() != 0) {
        // Assert non-torn read
        EXPECT_EQ(read.meter.value(), read.next_hop.value());
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  reader.join();
}

TEST(ScopeTableTest, LookupAndPublish) {
  ScopeTable table(1024);
  EXPECT_EQ(1024u, table.capacity());

  // Valid publication
  ScopeId s1(42);
  ScopePlan plan1{.meter = MeterId(5), .next_hop = NextHopId(12)};
  table.Publish(s1, plan1);

  ScopePlan read1 = table.Read(s1);
  EXPECT_EQ(5u, read1.meter.value());
  EXPECT_EQ(12u, read1.next_hop.value());

  // Invalid IDs return empty plan
  ScopePlan read_zero = table.Read(ScopeId(0));
  EXPECT_EQ(0u, read_zero.meter.value());
  EXPECT_EQ(0u, read_zero.next_hop.value());

  ScopePlan read_out_of_bounds = table.Read(ScopeId(2000));
  EXPECT_EQ(0u, read_out_of_bounds.meter.value());
  EXPECT_EQ(0u, read_out_of_bounds.next_hop.value());
}

}  // namespace
