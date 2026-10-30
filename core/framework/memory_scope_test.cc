// SPDX-License-Identifier: BSD-3-Clause

// Module memory scopes (Decision D-198): admitted before allocation, refused
// whole, released with the owner, accounted per module and resource.

#include "framework/memory_scope.h"

#include <gtest/gtest.h>

#include <malloc.h>
#include <rte_malloc.h>

#include <map>
#include <vector>

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
    policy.fallback = false;
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

// D-206: the framework, not the module, decides where admitted bytes land.
// DPDK's native heap is accounted as hugepages here (the test binaries'
// EAL runs on normal pages), so this is hugepage backing as bessd's auto
// mode resolves it on a host with hugepages: no cap, normal pages allowed.
class NativeAsHugepages {
 public:
  explicit NativeAsHugepages(MemoryPolicy policy = MemoryPolicy{}) {
    bess::utils::MemoryNode();
    bess::utils::detail::SetNativeHeapIsHugepagesForTest(true);
    bess::utils::SetMemoryPolicy(std::move(policy));
  }
  ~NativeAsHugepages() {
    bess::utils::SetMemoryPolicy(MemoryPolicy{});
    bess::utils::detail::SetNativeHeapIsHugepagesForTest(false);
  }
  NativeAsHugepages(const NativeAsHugepages &) = delete;
  NativeAsHugepages &operator=(const NativeAsHugepages &) = delete;
};

// The memory node heap's largest free block less `room`, taken unarmed (no
// admission), so the next allocation larger than `room` cannot fit there.
class FillNativeHeap {
 public:
  explicit FillNativeHeap(size_t room) {
    rte_malloc_socket_stats stats{};
    EXPECT_EQ(rte_malloc_get_socket_stats(bess::utils::MemoryNode(), &stats), 0);
    if (stats.greatest_free_size > room) {
      filler_ = rte_malloc_socket("test", stats.greatest_free_size - room, 64,
                                  bess::utils::MemoryNode());
    }
  }
  ~FillNativeHeap() { rte_free(filler_); }
  FillNativeHeap(const FillNativeHeap &) = delete;
  FillNativeHeap &operator=(const FillNativeHeap &) = delete;

 private:
  void *filler_ = nullptr;
};

TEST(MemoryScopeTest, PlacementIsHugepagesFirstThenNormalPagesAsTheyLand) {
  NativeAsHugepages backing;
  const std::string name = "scope_place";
  MemoryScope scope(&name);
  const size_t hugepage_tables = bess::utils::HugepageTableBytes();
  const uint64_t spilled = bess::utils::GetMemoryStatus().spilled_bytes;

  // Room on the memory node: the DPDK part is on hugepages, the glibc part
  // is reported as glibc.
  MemoryLease small = scope.Lease("table", kMB + 4096, 4096);
  void *a = bess::utils::DpdkAllocate(kMB, 64);
  std::optional<ModuleMemoryUse> row = Row(name, "table");
  ASSERT_TRUE(row);
  EXPECT_EQ(row->hugepage_bytes, kMB);
  EXPECT_EQ(row->normal_bytes, 0u);
  EXPECT_EQ(row->glibc_bytes, 4096u);

  {
    // The heap runs out before any cap: the growth spills to normal pages
    // and the account says so, with the hugepage count given back.
    FillNativeHeap full(4 * kMB);
    const size_t fallback_alloc =
        bess::utils::GetMemoryStatus().fallback_heap_alloc_bytes;
    small.growth()->Grow(8 * kMB);
    void *b = bess::utils::DpdkAllocate(8 * kMB, 64);
    row = Row(name, "table");
    EXPECT_EQ(row->hugepage_bytes, kMB);
    EXPECT_EQ(row->normal_bytes, 8 * kMB);
    EXPECT_EQ(row->admitted_bytes, 9 * kMB + 4096);
    const auto status = bess::utils::GetMemoryStatus();
    EXPECT_EQ(status.spilled_bytes, spilled + 8 * kMB);
    EXPECT_GE(status.fallback_heap_alloc_bytes, fallback_alloc + 8 * kMB);
    EXPECT_EQ(bess::utils::HugepageTableBytes(), hugepage_tables + kMB);
    bess::utils::DpdkFree(b);
    small.growth()->Shrink(8 * kMB);
  }
  bess::utils::DpdkFree(a);
  small.reset();
  EXPECT_EQ(bess::utils::HugepageTableBytes(), hugepage_tables);
}

TEST(MemoryScopeTest, PastTheCapBytesGoToNormalPagesWhileTheHeapHasRoom) {
  MemoryPolicy policy;
  policy.hugepage_cap_bytes = bess::utils::HugepageTableBytes() + kMB;
  NativeAsHugepages backing(policy);
  const std::string name = "scope_cap";
  MemoryScope scope(&name);
  const size_t fallback_alloc =
      bess::utils::GetMemoryStatus().fallback_heap_alloc_bytes;
  rte_malloc_socket_stats before{};
  ASSERT_EQ(rte_malloc_get_socket_stats(bess::utils::MemoryNode(), &before), 0);

  MemoryLease lease = scope.Lease("table", 2 * kMB);
  void *p = bess::utils::DpdkAllocate(2 * kMB, 64);
  const std::optional<ModuleMemoryUse> row = Row(name, "table");
  ASSERT_TRUE(row);
  EXPECT_EQ(row->hugepage_bytes, 0u);
  EXPECT_EQ(row->normal_bytes, 2 * kMB);
  // Taken from the normal-page heap, not from the memory node's heap,
  // which had room: the cap is spent.
  EXPECT_GE(bess::utils::GetMemoryStatus().fallback_heap_alloc_bytes,
            fallback_alloc + 2 * kMB);
  rte_malloc_socket_stats after{};
  ASSERT_EQ(rte_malloc_get_socket_stats(bess::utils::MemoryNode(), &after), 0);
  EXPECT_LT(after.heap_allocsz_bytes, before.heap_allocsz_bytes + 2 * kMB);
  bess::utils::DpdkFree(p);
}

TEST(MemoryScopeTest, ForbiddenNormalPagesRefuseWhenTheHeapRunsOut) {
  MemoryPolicy policy;
  policy.fallback = false;
  NativeAsHugepages backing(policy);
  const std::string name = "scope_forbid";
  MemoryScope scope(&name);
  FillNativeHeap full(4 * kMB);
  MemoryLease lease = scope.Lease("table", 8 * kMB);
  try {
    bess::utils::DpdkAllocate(8 * kMB, 64);
    FAIL() << "normal pages taken with --mem_fallback=false";
  } catch (const MemoryRefused &e) {
    EXPECT_EQ(e.reason(), MemoryRefused::Reason::kHugepagesExhausted);
    EXPECT_NE(std::string(e.what()).find("--mem_fallback=false"),
              std::string::npos);
  }
}

// Every glibc block an AdmittedGlibcAllocator<T> holds, as malloc reports it
// (malloc_usable_size and the chunk's size word), summed beside the
// allocator's own charge: no process-wide measurement, so no other thread's
// malloc can move it.
size_t blocks_held = 0;
template <typename T>
struct BlockCounting : bess::utils::AdmittedGlibcAllocator<T> {
  using Base = bess::utils::AdmittedGlibcAllocator<T>;
  using Base::Base;
  template <typename U>
  BlockCounting(const BlockCounting<U> &o) noexcept : Base(o) {}
  T *allocate(size_t n) {
    T *p = Base::allocate(n);
    blocks_held += malloc_usable_size(p) + sizeof(size_t);
    return p;
  }
  void deallocate(T *p, size_t n) noexcept {
    blocks_held -= malloc_usable_size(p) + sizeof(size_t);
    Base::deallocate(p, n);
  }
};

// Class C (D-206): control-plane containers stay on glibc, and what they hold
// is admitted exactly -- malloc's chunks, header and rounding included, as
// glibc reports each block -- so the account tracks the heap, not
// n * sizeof(T).
TEST(MemoryScopeTest, AdmittedGlibcAllocatorCountsMallocChunks) {
  const std::string name = "scope_glibc";
  MemoryScope scope(&name);
  MemoryLease lease = scope.Lease("mirror", 0);
  using Value = std::pair<const uint32_t, uint64_t>;
  using Map = std::map<uint32_t, uint64_t, std::less<>, BlockCounting<Value>>;
  Map map{Map::allocator_type(lease.growth())};
  for (uint32_t i = 0; i < 1000; i++) {
    map.emplace(i, i);
  }
  EXPECT_EQ(lease.bytes(), blocks_held);
  EXPECT_GT(lease.bytes(), 1000 * sizeof(Value));
  const std::optional<ModuleMemoryUse> row = Row(name, "mirror");
  ASSERT_TRUE(row);
  EXPECT_EQ(row->glibc_bytes, blocks_held);
  EXPECT_EQ(row->hugepage_bytes + row->normal_bytes, 0u);
  map.clear();
  EXPECT_EQ(blocks_held, 0u);
  EXPECT_EQ(lease.bytes(), 0u);
}

// Past the limit a control-plane insert is refused before it allocates: the
// container and its account are as they were.
TEST(MemoryScopeTest, AdmittedGlibcGrowthIsRefusedAtTheLimit) {
  const std::string name = "scope_glibc_limit";
  MemoryScope scope(&name);
  MemoryLease lease = scope.Lease("mirror", 0);
  using Vector =
      std::vector<uint64_t, bess::utils::AdmittedGlibcAllocator<uint64_t>>;
  Vector rows{Vector::allocator_type(lease.growth())};
  rows.reserve(1024);
  const size_t admitted = lease.bytes();
  EXPECT_EQ(admitted, bess::utils::MallocBytes(rows.data()));
  EXPECT_LE(admitted, bess::utils::MallocChunkBytes(1024 * sizeof(uint64_t)));
  TightPolicy tight(kMB);
  EXPECT_THROW(rows.reserve(size_t{16} << 20), MemoryRefused);
  EXPECT_EQ(rows.capacity(), 1024u);
  EXPECT_EQ(lease.bytes(), admitted);
}

// Class P (D-206): a container a worker reads takes the framework allocator,
// DPDK memory on the memory node admitted as it grows, and is reported
// where it landed.
TEST(MemoryScopeTest, DpdkAllocatorAdmitsAndPlacesItsGrowth) {
  NativeAsHugepages backing;
  const std::string name = "scope_dpdk_vector";
  MemoryScope scope(&name);
  MemoryLease lease = scope.Lease("hops", 0);
  using Vector = std::vector<uint64_t, bess::utils::DpdkAllocator<uint64_t>>;
  Vector hops{Vector::allocator_type(bess::utils::MemoryNode(), lease.growth())};
  hops.reserve(4096);
  EXPECT_EQ(lease.bytes(), 4096 * sizeof(uint64_t));
  const std::optional<ModuleMemoryUse> row = Row(name, "hops");
  ASSERT_TRUE(row);
  EXPECT_EQ(row->hugepage_bytes, 4096 * sizeof(uint64_t));
  EXPECT_EQ(row->glibc_bytes, 0u);
  hops = Vector(Vector::allocator_type(bess::utils::MemoryNode(), lease.growth()));
  EXPECT_EQ(lease.bytes(), 0u);
}

}  // namespace
