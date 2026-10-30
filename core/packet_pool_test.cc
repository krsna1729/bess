// SPDX-License-Identifier: BSD-3-Clause

// The packet pool under hugepage backing (D-206): hugepages first, the rest
// on normal pages rather than a smaller pool or a fatal error, unless normal
// pages are forbidden; reported as bessd/packet_pool by where it landed.
// DPDK's native heap stands in for the hugepage heap here (the test
// binaries' EAL runs on normal pages).

#include "packet_pool.h"

#include <gtest/gtest.h>

#include <rte_malloc.h>

#include <optional>
#include <string>
#include <vector>

#include "framework/memory_scope.h"
#include "utils/dpdk_memory.h"

namespace bess {
namespace {

constexpr size_t kMB = size_t{1} << 20;
constexpr size_t kPackets = 8191;

class NativeAsHugepages {
 public:
  explicit NativeAsHugepages(utils::MemoryPolicy policy) {
    utils::MemoryNode();
    utils::detail::SetNativeHeapIsHugepagesForTest(true);
    utils::SetMemoryPolicy(std::move(policy));
  }
  ~NativeAsHugepages() {
    utils::SetMemoryPolicy(utils::MemoryPolicy{});
    utils::detail::SetNativeHeapIsHugepagesForTest(false);
  }
  NativeAsHugepages(const NativeAsHugepages &) = delete;
  NativeAsHugepages &operator=(const NativeAsHugepages &) = delete;
};

// Leaves `room` of the memory node heap's largest free block, so the pool
// (about 20 MB) cannot fit there whole.
class FillNativeHeap {
 public:
  explicit FillNativeHeap(size_t room) {
    rte_malloc_socket_stats stats{};
    EXPECT_EQ(rte_malloc_get_socket_stats(utils::MemoryNode(), &stats), 0);
    if (stats.greatest_free_size > room) {
      filler_ = rte_malloc_socket("test", stats.greatest_free_size - room, 64,
                                  utils::MemoryNode());
    }
  }
  ~FillNativeHeap() { rte_free(filler_); }
  FillNativeHeap(const FillNativeHeap &) = delete;
  FillNativeHeap &operator=(const FillNativeHeap &) = delete;

 private:
  void *filler_ = nullptr;
};

std::optional<framework::ModuleMemoryUse> PoolRow() {
  for (const framework::ModuleMemoryUse &row :
       framework::MemoryAccounts::List()) {
    if (row.module == "bessd" && row.resource == "packet_pool") {
      return row;
    }
  }
  return std::nullopt;
}

// Every packet the pool holds can be taken and given back.
void ExpectEveryPacketUsable(PacketPool &pool) {
  std::vector<PacketHandle> packets(pool.Capacity());
  ASSERT_TRUE(pool.AllocBulk(packets.data(), packets.size(), 64));
  for (PacketHandle pkt : packets) {
    rte_pktmbuf_free(pkt);
  }
}

TEST(PacketPoolTest, OverflowsToNormalPagesWhenHugepagesRunOut) {
  NativeAsHugepages backing{utils::MemoryPolicy{}};
  std::optional<DpdkPacketPool> pool;
  {
    FillNativeHeap full(8 * kMB);
    pool.emplace(kPackets, utils::MemoryNode());
  }
  EXPECT_EQ(pool->Capacity(), kPackets);
  const auto row = PoolRow();
  ASSERT_TRUE(row);
  EXPECT_GT(row->hugepage_bytes, 0u);
  EXPECT_GT(row->normal_bytes, 0u);
  EXPECT_EQ(row->glibc_bytes, 0u);
  ExpectEveryPacketUsable(*pool);
  pool.reset();
  EXPECT_FALSE(PoolRow());
}

TEST(PacketPoolTest, WithNormalPagesForbiddenThePoolIsSmaller) {
  utils::MemoryPolicy policy;
  policy.fallback = false;
  NativeAsHugepages backing(policy);
  std::optional<DpdkPacketPool> pool;
  {
    FillNativeHeap full(8 * kMB);
    pool.emplace(kPackets, utils::MemoryNode());
  }
  EXPECT_GT(pool->Capacity(), 0u);
  EXPECT_LT(pool->Capacity(), kPackets);
  const auto row = PoolRow();
  ASSERT_TRUE(row);
  EXPECT_EQ(row->normal_bytes, 0u);
  ExpectEveryPacketUsable(*pool);
}

TEST(PacketPoolTest, PastTheCapThePoolIsOnNormalPages) {
  utils::MemoryPolicy policy;
  policy.hugepage_cap_bytes = utils::HugepageTableBytes() + kMB;
  NativeAsHugepages backing(policy);
  DpdkPacketPool pool(kPackets, utils::MemoryNode());
  EXPECT_EQ(pool.Capacity(), kPackets);
  const auto row = PoolRow();
  ASSERT_TRUE(row);
  EXPECT_EQ(row->hugepage_bytes, 0u);
  EXPECT_GT(row->normal_bytes, 0u);
  ExpectEveryPacketUsable(pool);
}

}  // namespace
}  // namespace bess
