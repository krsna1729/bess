// SPDX-License-Identifier: BSD-3-Clause

// glibc's heap in bessd (RT-1): the arena cap holds however many threads
// allocate at once, and free bytes that never went through admission trim
// glibc once they pass the bound.

#include "runtime/malloc_policy.h"

#include <malloc.h>

#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace bess::runtime {
namespace {

// The arenas glibc has made: one <heap> element each in malloc_info.
size_t MallocHeaps() {
  char *text = nullptr;
  size_t size = 0;
  FILE *out = open_memstream(&text, &size);
  if (out == nullptr || malloc_info(0, out) != 0) {
    ADD_FAILURE() << "malloc_info";
  }
  if (out != nullptr) {
    EXPECT_EQ(0, fclose(out));  // flushes the stream into `text`
  }
  const std::string info(text == nullptr ? "" : text, size);
  free(text);
  size_t heaps = 0;
  for (size_t at = info.find("<heap nr="); at != std::string::npos;
       at = info.find("<heap nr=", at + 1)) {
    heaps++;
  }
  return heaps;
}

// Leaves `bytes` free in glibc that no admission saw, as a teardown's
// transient copies do: 4 KiB blocks (below the mmap threshold and tcache's
// sizes) freed behind the last one, kept until the test ends, so they cannot
// merge into the top chunk, which glibc would hand back on its own.
void *Garbage(size_t bytes) {
  std::vector<void *> blocks(bytes / 4096 + 1);
  for (void *&block : blocks) {
    block = malloc(4096);
  }
  void *keep = blocks.back();
  blocks.pop_back();
  for (void *block : blocks) {
    free(block);
  }
  return keep;
}

// 32 threads, each holding 100 allocations while all the others hold theirs
// (as 32 concurrent Applies would): uncapped, glibc gives each its own arena.
TEST(MallocPolicyTest, ArenasStayAtTheCapUnderConcurrentAllocation) {
  CapMallocArenas();  // before this test's first thread, as in main()
  constexpr int kThreads = 32;
  constexpr int kAllocations = 100;
  std::barrier all_hold(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&all_hold] {
      std::vector<void *> blocks;
      blocks.reserve(kAllocations);
      for (int i = 0; i < kAllocations; i++) {
        blocks.push_back(malloc(1024));
      }
      all_hold.arrive_and_wait();
      for (void *block : blocks) {
        free(block);
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  EXPECT_LE(MallocHeaps(), static_cast<size_t>(kMallocArenas));
}

TEST(MallocPolicyTest, FreeBytesPastTheBoundTrimOncePerPeriod) {
  constexpr size_t kMiB = size_t{1024} * 1024;
  constexpr size_t kBound = 16 * kMiB;
  MallocFreeCheck check(std::chrono::seconds(1));
  auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  (void)check.Check(now, kBound);  // the base
  const uint64_t trims = malloc_trims();

  void *keep = Garbage(2 * kBound);
  EXPECT_FALSE(check.Check(now + std::chrono::milliseconds(500), kBound)) << "within the period";
  EXPECT_EQ(trims, malloc_trims());
  now += std::chrono::seconds(1);
  EXPECT_TRUE(check.Check(now, kBound));
  EXPECT_EQ(trims + 1, malloc_trims());
  now += std::chrono::seconds(1);
  EXPECT_FALSE(check.Check(now, kBound)) << "the same free bytes, already trimmed";
  EXPECT_EQ(trims + 1, malloc_trims());

  void *keep_more = Garbage(kBound / 2);
  now += std::chrono::seconds(1);
  EXPECT_FALSE(check.Check(now, kBound)) << "half the bound";
  EXPECT_EQ(trims + 1, malloc_trims());
  free(keep_more);
  free(keep);
}

TEST(MallocPolicyTest, FreeBoundIsAThirtySecondOfTheLimitAtLeast64MiB) {
  constexpr size_t kGiB = size_t{1024} * 1024 * 1024;
  EXPECT_EQ(kMallocTrimAfterBytes, MallocFreeBound(0));
  EXPECT_EQ(kMallocTrimAfterBytes, MallocFreeBound(kGiB));
  EXPECT_EQ(4 * kGiB / 32, MallocFreeBound(4 * kGiB));
}

}  // namespace
}  // namespace bess::runtime
