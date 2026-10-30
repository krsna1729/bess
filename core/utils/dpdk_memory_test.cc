// SPDX-License-Identifier: BSD-3-Clause

// Dataplane memory policy (Decision D-198): refusals instead of OOM kills,
// normal pages only when declared. Tests run DPDK as the test binaries bring
// it up, lazily and normally without hugepages.

#include "utils/dpdk_memory.h"

#include "classifier/rte_hash_exact.h"
#include "framework/memory_scope.h"

#include <gtest/gtest.h>

#include <rte_eal.h>
#include <rte_hash.h>
#include <rte_malloc.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    // Hugepages with --mem_fallback=false: refused, naming the flag.
    MemoryPolicy forbid;
    forbid.fallback = false;
    PolicyScope scope(forbid);
    try {
      FallbackSocket(kMB);
      FAIL() << "fallback with --mem_fallback=false";
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
  policy.fallback = false;
  {
    PolicyScope scope(policy);
    const size_t before = HugepageTableBytes();
    ASSERT_LE(before, 4 * kMB) << "tables admitted by an earlier test";
    const size_t room = policy.hugepage_cap_bytes - before;
    EXPECT_EQ(AdmitTable(room), TableBacking::kHugepages);
    EXPECT_EQ(HugepageTableBytes(), before + room);
    // Past the cap with --mem_fallback=false: refused, nothing counted.
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
    // Past the cap with normal pages allowed: normal pages, not counted.
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
  policy.fallback = false;
  const size_t limit = policy.limit_bytes;
  {
    PolicyScope scope(policy);
    const auto status = GetMemoryStatus();
    EXPECT_EQ(status.hugepages, rte_eal_has_hugepages() != 0);
    EXPECT_FALSE(status.fallback);
    EXPECT_EQ(status.limit_bytes, limit);
    EXPECT_EQ(status.reserve_bytes, 128 * kMB);
    EXPECT_EQ(status.limit_source, "test");
    EXPECT_GT(status.used_bytes, 0u);
    EXPECT_GT(status.dpdk_heap_bytes, 0u);
  }
  // The default (D-206): normal pages past the hugepages.
  const auto status = GetMemoryStatus();
  EXPECT_EQ(status.limit_bytes, 0u);
  EXPECT_TRUE(status.fallback);
}

size_t Rss() {
  size_t pages = 0, resident = 0;
  FILE *f = std::fopen("/proc/self/statm", "r");
  EXPECT_NE(f, nullptr);
  EXPECT_EQ(std::fscanf(f, "%zu %zu", &pages, &resident), 2);
  std::fclose(f);
  return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

// oom_kill of this process's cgroup v2 memory.events; 0 if unreadable.
uint64_t OomKills() {
  std::ifstream cgroup("/proc/self/cgroup");
  std::string line;
  std::getline(cgroup, line);  // "0::/path" under cgroup v2
  std::ifstream events("/sys/fs/cgroup" + line.substr(line.find("::") + 2) +
                       "/memory.events");
  std::string key;
  uint64_t value = 0;
  while (events >> key >> value) {
    if (key == "oom_kill") {
      return value;
    }
  }
  return 0;
}

// A table on the --no-huge heap is resident from its allocation, right after
// its admission, so freeing it never grows RSS (D-206): DPDK zeroes a clean
// element on free, which used to fault an admitted but untouched table in at
// teardown and OOM-kill bessd near its cgroup limit.
TEST(DpdkMemoryTest, FreeNeverGrowsResidentMemory) {
  ASSERT_EQ(rte_eal_has_hugepages(), 0) << "tests run on normal pages";
  constexpr size_t kBytes = 64 * kMB;
  const MemoryPolicy policy = TightPolicy(kBytes + 32 * kMB, 0);
  PolicyScope scope(policy);
  const size_t before = Rss();
  void *table = DpdkAllocate(kBytes, 64);
  const size_t allocated = Rss();
  EXPECT_GE(allocated, before + kBytes * 9 / 10)
      << "admitted pages are resident from the allocation";
  DpdkFree(table);
  EXPECT_LE(Rss(), allocated + kMB) << "the free faulted pages in";
}

// What a DPDK library allocates internally (rte_hash: rte_zmalloc, which
// leaves clean memory untouched) cannot be prefaulted, so it is counted as
// allocated (D-206): under a tight limit an admission that RSS alone would
// let through is refused while the table is held, and freeing the table --
// which faults it in -- stays within what was admitted. DPDK's heap gives
// no page back, so RSS after the free is its peak.
TEST(DpdkMemoryTest, LibraryTableFreeStaysWithinTheLimit) {
  ASSERT_EQ(rte_eal_has_hugepages(), 0) << "tests run on normal pages";
  rte_hash_parameters params{};
  params.name = "dpdk_memory_test";
  params.entries = 2u << 20;
  params.key_len = 16;
  params.socket_id = bess::utils::MemoryNode();
  const size_t admitted =
      bess::classifier::detail::RteHashBytes(params.entries, params.key_len);
  const MemoryPolicy policy = TightPolicy(admitted + 64 * kMB, 0);
  PolicyScope scope(policy);
  const uint64_t oom_kills = OomKills();
  const size_t before = Rss();
  rte_hash *table = bess::classifier::detail::CreateRteHash(params);
  ASSERT_NE(table, nullptr);
  const size_t created = Rss();
  // Room by RSS alone, less a margin for the test's own glibc: refused, as
  // the table's unfaulted bytes count.
  const size_t by_rss = policy.limit_bytes - created - 4 * kMB;
  EXPECT_THROW(AdmitNormalMemory(by_rss), MemoryRefused)
      << "allocated but unfaulted DPDK memory was not counted";
  rte_hash_free(table);
  EXPECT_LE(Rss(), before + admitted)
      << "freeing the table grew RSS past what was admitted";
  EXPECT_EQ(OomKills(), oom_kills);
}

// Predicted glibc costs (D-206: Router::GlibcBytes and every MemoryCost that
// sums MallocChunkBytes) are right exactly when MallocChunkBytes is the
// chunk malloc takes: checked on a block of every request size up to a page,
// each measured on itself (MallocBytes), so no other thread's malloc moves
// it.
TEST(DpdkMemoryTest, MallocChunkBytesIsWhatMallocTakes) {
  for (size_t n = 1; n <= 4096; n++) {
    void *p = std::malloc(n);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(bess::utils::MallocBytes(p), bess::utils::MallocChunkBytes(n))
        << n << " bytes";
    std::free(p);
  }
}

}  // namespace
