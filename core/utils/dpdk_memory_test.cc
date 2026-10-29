// SPDX-License-Identifier: BSD-3-Clause

// Dataplane memory policy (Decision D-198): refusals instead of OOM kills,
// normal pages only when declared. Tests run DPDK as the test binaries bring
// it up, lazily and normally without hugepages.

#include "utils/dpdk_memory.h"

#include "framework/memory_scope.h"

#include <gtest/gtest.h>

#include <rte_eal.h>
#include <rte_malloc.h>

#include <new>
#include <string>

namespace {

using bess::utils::AdmitNormalMemory;
using bess::utils::AdmitTable;
using bess::utils::DpdkAllocate;
using bess::utils::DpdkFree;
using bess::utils::FallbackSocket;
using bess::utils::GetMemoryStatus;
using bess::utils::HugepageTableBytes;
using bess::utils::MemoryPolicy;
using bess::utils::MemoryRefused;
using bess::utils::ReleaseTable;
using bess::utils::SetMemoryPolicy;
using bess::utils::TableBacking;

constexpr size_t kMB = size_t{1} << 20;

// Sets a policy for one test and restores the default after it.
class PolicyScope {
 public:
  explicit PolicyScope(MemoryPolicy policy) {
    bess::utils::MemoryNode();  // DPDK up first, sized without this policy
    SetMemoryPolicy(std::move(policy));
  }
  ~PolicyScope() { SetMemoryPolicy(MemoryPolicy{}); }
  PolicyScope(const PolicyScope &) = delete;
  PolicyScope &operator=(const PolicyScope &) = delete;
};

// A limit `headroom` above what the process uses now, `reserve` of it kept.
MemoryPolicy TightPolicy(size_t headroom, size_t reserve) {
  MemoryPolicy policy;
  policy.limit_bytes = GetMemoryStatus().used_bytes + headroom;
  policy.reserve_bytes = reserve;
  policy.limit_source = "test";
  return policy;
}

TEST(DpdkMemoryTest, MemoryRefusedCarriesReasonBytesAndMessage) {
  const MemoryRefused refused(MemoryRefused::Reason::kNormalLimit, 4096,
                              "memory limit: no");
  EXPECT_EQ(refused.reason(), MemoryRefused::Reason::kNormalLimit);
  EXPECT_EQ(refused.bytes(), 4096u);
  EXPECT_STREQ(refused.what(), "memory limit: no");
  // Existing std::bad_alloc handling still catches it.
  try {
    throw MemoryRefused(MemoryRefused::Reason::kHugepagesExhausted, 1, "x");
  } catch (const std::bad_alloc &e) {
    EXPECT_STREQ(e.what(), "x");
  }
}

TEST(DpdkMemoryTest, AdmitNormalMemoryRefusesPastLimitLessReserve) {
  // Without a limit everything is admitted.
  EXPECT_NO_THROW(AdmitNormalMemory(size_t{1} << 40));

  PolicyScope scope(TightPolicy(256 * kMB, 128 * kMB));
  const uint64_t refusals = GetMemoryStatus().refusals;
  EXPECT_NO_THROW(AdmitNormalMemory(1 * kMB));
  try {
    AdmitNormalMemory(192 * kMB);  // within the limit, not past the reserve
    FAIL() << "admitted past limit - reserve";
  } catch (const MemoryRefused &e) {
    EXPECT_EQ(e.reason(), MemoryRefused::Reason::kNormalLimit);
    EXPECT_EQ(e.bytes(), 192 * kMB);
    EXPECT_NE(std::string(e.what()).find("test"), std::string::npos);
  }
  EXPECT_THROW(AdmitNormalMemory(SIZE_MAX), MemoryRefused);
  EXPECT_EQ(GetMemoryStatus().refusals, refusals + 2);
}

TEST(DpdkMemoryTest, NoHugeAllocationPastTheLimitIsRefused) {
  bess::utils::MemoryNode();
  if (rte_eal_has_hugepages()) {
    GTEST_SKIP() << "DPDK has hugepages (BESSD_MEM_BACKING)";
  }
  PolicyScope scope(TightPolicy(64 * kMB, 32 * kMB));
  void *small = DpdkAllocate(4096, 64);
  ASSERT_NE(small, nullptr);
  DpdkFree(small);
  try {
    DpdkAllocate(48 * kMB, 64);
    FAIL() << "allocated past limit - reserve";
  } catch (const MemoryRefused &e) {
    EXPECT_EQ(e.reason(), MemoryRefused::Reason::kNormalLimit);
    EXPECT_EQ(e.bytes(), 48 * kMB);
  }
}

TEST(DpdkMemoryTest, FallbackHeapOnlyWhereAllowed) {
  bess::utils::MemoryNode();
  if (rte_eal_has_hugepages()) {
    // Hugepages without --mem_fallback: refused, naming the flag.
    try {
      FallbackSocket(kMB);
      FAIL() << "fallback without --mem_fallback";
    } catch (const MemoryRefused &e) {
      EXPECT_EQ(e.reason(), MemoryRefused::Reason::kHugepagesExhausted);
      EXPECT_NE(std::string(e.what()).find("--mem_fallback"),
                std::string::npos);
    }
    return;
  }
  // Under --no-huge the fallback heap extends DPDK's fixed one.
  const int socket = FallbackSocket(kMB);
  EXPECT_NE(socket, bess::utils::MemoryNode());
  void *p = DpdkAllocate(kMB, 64, socket);
  ASSERT_NE(p, nullptr);
  const auto status = GetMemoryStatus();
  EXPECT_GE(status.fallback_heap_bytes, 64 * kMB);
  EXPECT_GE(status.fallback_heap_alloc_bytes, kMB);
  DpdkFree(p);
}

// AdmitTable (D-198): the policy-aware admission MemoryScope calls before a
// table is allocated.
TEST(DpdkMemoryTest, AdmitTableFollowsTheBacking) {
  bess::utils::MemoryNode();
  if (!rte_eal_has_hugepages()) {
    // Normal pages: admitted against the limit, nothing counted.
    PolicyScope scope(TightPolicy(64 * kMB, 32 * kMB));
    EXPECT_EQ(AdmitTable(4096), TableBacking::kNormal);
    EXPECT_EQ(HugepageTableBytes(), 0u);
    EXPECT_THROW(AdmitTable(48 * kMB), MemoryRefused);
    return;
  }
  MemoryPolicy policy;
  policy.hugepage_cap_bytes = 8 * kMB;
  {
    PolicyScope scope(policy);
    const size_t before = HugepageTableBytes();
    ASSERT_LE(before, 4 * kMB) << "tables admitted by an earlier test";
    const size_t room = policy.hugepage_cap_bytes - before;
    EXPECT_EQ(AdmitTable(room), TableBacking::kHugepages);
    EXPECT_EQ(HugepageTableBytes(), before + room);
    // Past the cap without --mem_fallback: refused, nothing counted.
    try {
      AdmitTable(1);
      FAIL() << "admitted past --mem_hugepage_mb";
    } catch (const MemoryRefused &e) {
      EXPECT_EQ(e.reason(), MemoryRefused::Reason::kHugepagesExhausted);
      EXPECT_NE(std::string(e.what()).find("--mem_fallback"),
                std::string::npos);
    }
    EXPECT_EQ(HugepageTableBytes(), before + room);
    ReleaseTable(TableBacking::kHugepages, room);
    EXPECT_EQ(HugepageTableBytes(), before);
  }
  policy.fallback = true;
  {
    PolicyScope scope(policy);
    const size_t before = HugepageTableBytes();
    // Past the cap with --mem_fallback: normal pages, not counted.
    EXPECT_EQ(AdmitTable(16 * kMB), TableBacking::kNormal);
    EXPECT_EQ(HugepageTableBytes(), before);
    ReleaseTable(TableBacking::kNormal, 16 * kMB);
    EXPECT_EQ(HugepageTableBytes(), before);
  }
}

// Where hugepages are charged to memory.max (cgroup2
// memory_hugetlb_accounting, D-198), a table is also admitted against the
// limit for the whole pages the heap must map for it; nothing if the heap
// already holds it.
TEST(DpdkMemoryTest, HugepageChargeIsTheWholePagesTheHeapMustAdd) {
  using bess::utils::detail::HugepageCharge;
  constexpr size_t kGB = size_t{1} << 30;
  EXPECT_EQ(HugepageCharge(kMB, 2 * kMB, kGB), 0u);
  EXPECT_EQ(HugepageCharge(2 * kMB, 2 * kMB, kGB), 0u);
  EXPECT_EQ(HugepageCharge(2 * kMB + 1, 2 * kMB, kGB), kGB);
  EXPECT_EQ(HugepageCharge(kGB + 1, 0, kGB), 2 * kGB);
  EXPECT_EQ(HugepageCharge(3 * kMB, 0, 2 * kMB), 4 * kMB);
  EXPECT_EQ(HugepageCharge(3 * kMB, 0, 0), 3 * kMB);  // page size unknown
}

TEST(DpdkMemoryTest, ChargedHugepagesAreAdmittedAgainstTheLimit) {
  bess::utils::MemoryNode();
  if (!rte_eal_has_hugepages()) {
    GTEST_SKIP() << "DPDK runs on normal pages (BESSD_MEM_BACKING)";
  }
  rte_malloc_socket_stats stats{};
  ASSERT_EQ(rte_malloc_get_socket_stats(bess::utils::MemoryNode(), &stats), 0);
  // More than the heap holds, within no cap: new pages must be mapped.
  const size_t bytes = stats.greatest_free_size + kMB;
  MemoryPolicy policy = TightPolicy(kMB, 32 * kMB);
  {
    // Not charged: the limit does not see hugepages.
    PolicyScope scope(policy);
    EXPECT_EQ(AdmitTable(bytes), TableBacking::kHugepages);
    ReleaseTable(TableBacking::kHugepages, bytes);
  }
  policy.hugetlb_charged = true;
  {
    PolicyScope scope(policy);
    const size_t before = HugepageTableBytes();
    try {
      AdmitTable(bytes);
      FAIL() << "charged hugepages admitted past the limit";
    } catch (const MemoryRefused &e) {
      EXPECT_EQ(e.reason(), MemoryRefused::Reason::kNormalLimit);
    }
    EXPECT_EQ(HugepageTableBytes(), before) << "a refusal counts nothing";
    // What the heap already holds maps no page: admitted.
    if (stats.greatest_free_size > 0) {
      EXPECT_EQ(AdmitTable(stats.greatest_free_size), TableBacking::kHugepages);
      ReleaseTable(TableBacking::kHugepages, stats.greatest_free_size);
    }
  }
}

// DpdkArray states what it allocates (D-198: what a module admits for it).
TEST(DpdkMemoryTest, DpdkArrayBytesMatchesTheConstructor) {
  struct Stamp {
    uint64_t a, b;
  };
  for (size_t n : {size_t{1}, size_t{100}, size_t{65536}}) {
    const bess::utils::DpdkArray<Stamp> array(n);
    EXPECT_EQ(bess::framework::MemoryCost<bess::utils::DpdkArray<Stamp>>::Bytes(n),
              array.bytes());
    EXPECT_EQ(array.bytes(), n * sizeof(Stamp));
  }
}

TEST(DpdkMemoryTest, StatusReflectsThePolicy) {
  MemoryPolicy policy = TightPolicy(1024 * kMB, 128 * kMB);
  policy.fallback = true;
  const size_t limit = policy.limit_bytes;
  {
    PolicyScope scope(policy);
    const auto status = GetMemoryStatus();
    EXPECT_EQ(status.hugepages, rte_eal_has_hugepages() != 0);
    EXPECT_TRUE(status.fallback);
    EXPECT_EQ(status.limit_bytes, limit);
    EXPECT_EQ(status.reserve_bytes, 128 * kMB);
    EXPECT_EQ(status.limit_source, "test");
    EXPECT_GT(status.used_bytes, 0u);
    EXPECT_GT(status.dpdk_heap_bytes, 0u);
  }
  const auto status = GetMemoryStatus();
  EXPECT_EQ(status.limit_bytes, 0u);
  EXPECT_FALSE(status.fallback);
}

}  // namespace
