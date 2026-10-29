// SPDX-License-Identifier: BSD-3-Clause

// Module memory scopes (Decision D-198): admitted before allocation, refused
// whole, released with the owner, accounted per module and resource.

#include "framework/memory_scope.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace {

using bess::framework::MemoryAccounts;
using bess::framework::MemoryCost;
using bess::framework::MemoryLease;
using bess::framework::MemoryScope;
using bess::framework::ModuleMemoryUse;
using bess::framework::Sized;
using bess::utils::MemoryPolicy;
using bess::utils::MemoryRefused;

constexpr size_t kMB = size_t{1} << 20;

// A policy under which `room` more bytes are admitted and 64 MiB more are
// refused, whichever backing DPDK came up with: a tight limit for normal
// pages, a tight cap (and no fallback) for hugepages.
class TightPolicy {
 public:
  explicit TightPolicy(size_t room) {
    bess::utils::MemoryNode();  // DPDK up first, sized without this policy
    MemoryPolicy policy;
    policy.limit_bytes = bess::utils::GetMemoryStatus().used_bytes + room + 32 * kMB;
    policy.reserve_bytes = 32 * kMB;
    policy.limit_source = "test";
    policy.hugepage_cap_bytes = bess::utils::HugepageTableBytes() + room;
    bess::utils::SetMemoryPolicy(std::move(policy));
  }
  ~TightPolicy() { bess::utils::SetMemoryPolicy(MemoryPolicy{}); }
  TightPolicy(const TightPolicy &) = delete;
  TightPolicy &operator=(const TightPolicy &) = delete;
};

// A primitive as the scope sees one: a cost from its sizing argument, its
// own bytes(), and growth admitted through the hook.
class Table {
 public:
  static size_t Bytes(size_t entries) { return entries * 64; }
  static size_t Bytes(size_t entries, bool) { return Bytes(entries); }
  explicit Table(size_t entries, bool fail = false) : bytes_(Bytes(entries)) {
    if (fail) {
      throw std::runtime_error("constructor failed");
    }
  }
  size_t bytes() const { return bytes_; }
  void set_memory_growth(bess::utils::MemoryGrowth *growth) { growth_ = growth; }
  void Grow(size_t entries) {
    growth_->Grow(Bytes(entries));  // admitted first; a throw leaves it as is
    bytes_ += Bytes(entries);
  }

 private:
  size_t bytes_;
  bess::utils::MemoryGrowth *growth_ = nullptr;
};

std::optional<ModuleMemoryUse> Row(const std::string &module,
                                   const std::string &resource) {
  for (const ModuleMemoryUse &row : MemoryAccounts::List()) {
    if (row.module == module && row.resource == resource) {
      return row;
    }
  }
  return std::nullopt;
}

TEST(MemoryScopeTest, AdmitAndReleaseAreAccounted) {
  const std::string name = "scope_admit";
  MemoryScope scope(&name);
  EXPECT_EQ(scope.socket(), bess::utils::MemoryNode());
  EXPECT_FALSE(Row(name, "fteids"));

  scope.Admit("fteids", 4096);
  scope.Admit("fteids", 4096);
  scope.Admit("pdrs", 100);
  EXPECT_EQ(scope.admitted_bytes(), 8192u + 100);
  auto row = Row(name, "fteids");
  ASSERT_TRUE(row);
  EXPECT_EQ(row->admitted_bytes, 8192u);
  EXPECT_EQ(row->current_bytes, 8192u);

  scope.Release("fteids", 4096);
  EXPECT_EQ(Row(name, "fteids")->admitted_bytes, 4096u);
  scope.Release("fteids", 1 << 20);  // at most what it holds
  EXPECT_FALSE(Row(name, "fteids"));
  scope.Release("unknown", 1);  // nothing to give back
  EXPECT_EQ(scope.admitted_bytes(), 100u);
}

TEST(MemoryScopeTest, RefusalRecordsNothing) {
  const std::string name = "scope_refused";
  MemoryScope scope(&name);
  TightPolicy policy(8 * kMB);
  const size_t hugepage_tables = bess::utils::HugepageTableBytes();

  EXPECT_THROW(scope.Admit("big", 64 * kMB), MemoryRefused);
  EXPECT_THROW(scope.Lease("big", 64 * kMB), MemoryRefused);
  EXPECT_THROW(scope.Create<Table>("big", size_t{1} << 20), MemoryRefused);
  EXPECT_EQ(scope.admitted_bytes(), 0u);
  EXPECT_FALSE(Row(name, "big"));
  EXPECT_EQ(bess::utils::HugepageTableBytes(), hugepage_tables);

  // What fits is still admitted.
  scope.Admit("small", 4096);
  EXPECT_EQ(scope.admitted_bytes(), 4096u);
}

TEST(MemoryScopeTest, CreateAdmitsTheCostAndReleasesOnDestruction) {
  const std::string name = "scope_create";
  MemoryScope scope(&name);
  {
    Sized<Table> table = scope.Create<Table>("sessions", size_t{1000});
    ASSERT_TRUE(table);
    EXPECT_EQ(table.admitted_bytes(), MemoryCost<Table>::Bytes(size_t{1000}));
    EXPECT_EQ(table->bytes(), MemoryCost<Table>::Bytes(size_t{1000}));
    EXPECT_EQ(Row(name, "sessions")->admitted_bytes, 64000u);

    // Growth goes through the lease, and current follows bytes().
    table->Grow(10);
    EXPECT_EQ(table.admitted_bytes(), 64640u);
    EXPECT_EQ(Row(name, "sessions")->current_bytes, 64640u);

    // Moving the owner keeps the object and its admission.
    Table *object = table.get();
    Sized<Table> moved = std::move(table);
    EXPECT_EQ(moved.get(), object);
    EXPECT_EQ(Row(name, "sessions")->admitted_bytes, 64640u);
  }
  EXPECT_FALSE(Row(name, "sessions"));
  EXPECT_EQ(scope.admitted_bytes(), 0u);

  // A constructor that throws gives its admission back.
  EXPECT_THROW(scope.Create<Table>("sessions", size_t{10}, true),
               std::runtime_error);
  EXPECT_EQ(scope.admitted_bytes(), 0u);
}

// A factory that can fail with the module's own error: lease the cost
// first, make second, then adopt -- the same owner Create returns.
TEST(MemoryScopeTest, LeaseForThenAdoptIsCreateForAFactory) {
  const std::string name = "scope_adopt";
  MemoryScope scope(&name);
  MemoryLease lease = scope.LeaseFor<Table>("flows", size_t{100});
  EXPECT_EQ(lease.bytes(), MemoryCost<Table>::Bytes(size_t{100}));
  Sized<Table> table =
      MemoryScope::Adopt(std::move(lease), std::make_unique<Table>(size_t{100}));
  table->Grow(1);
  EXPECT_EQ(table.admitted_bytes(), 6464u);
  EXPECT_EQ(Row(name, "flows")->current_bytes, 6464u);
  table.reset();
  EXPECT_FALSE(Row(name, "flows"));
}

TEST(MemoryScopeTest, LeasesReleaseAndScopeReleasesTheRest) {
  const size_t hugepage_tables = bess::utils::HugepageTableBytes();
  const std::string name = "scope_lease";
  {
    MemoryScope scope(&name);
    MemoryLease lease = scope.Lease("ring", 8192);
    EXPECT_EQ(lease.bytes(), 8192u);
    lease.growth()->Grow(4096);
    lease.growth()->Shrink(8192);
    EXPECT_EQ(lease.bytes(), 4096u);
    lease.reset();
    EXPECT_FALSE(lease);
    EXPECT_FALSE(Row(name, "ring"));

    scope.Admit("table", 4096);
    MemoryLease kept = scope.Lease("table", 4096);
    EXPECT_EQ(Row(name, "table")->admitted_bytes, 8192u);
  }
  // The scope went first and released both; the lease then gave back
  // nothing twice.
  EXPECT_FALSE(Row(name, "table"));
  EXPECT_EQ(bess::utils::HugepageTableBytes(), hugepage_tables);
}

TEST(MemoryScopeTest, AccountsListEveryScopeByModuleAndResource) {
  const std::string a = "scope_list_a", b = "scope_list_b";
  MemoryScope scope_b(&b);
  MemoryScope scope_a(&a);
  scope_b.Admit("y", 3);
  scope_a.Admit("z", 2);
  scope_a.Admit("x", 1);
  const auto pool = Row("bessd", "packet_pool");
  const size_t pool_bytes = pool ? pool->admitted_bytes : 0;
  MemoryAccounts::Process().Admit("packet_pool", 5);

  std::vector<std::pair<std::string, std::string>> order;
  for (const ModuleMemoryUse &row : MemoryAccounts::List()) {
    if (row.module == a || row.module == b) {
      order.emplace_back(row.module, row.resource);
    }
  }
  const std::vector<std::pair<std::string, std::string>> expected = {
      {a, "x"}, {a, "z"}, {b, "y"}};
  EXPECT_EQ(order, expected);
  EXPECT_EQ(Row("bessd", "packet_pool")->admitted_bytes, pool_bytes + 5);
  MemoryAccounts::Process().Release("packet_pool", 5);
}

}  // namespace
