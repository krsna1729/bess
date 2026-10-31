// SPDX-License-Identifier: BSD-3-Clause

// glibc's heap in bessd (RT-1): the arena cap holds however many threads
// allocate at once.

#include "runtime/malloc_policy.h"

#include <malloc.h>

#include <barrier>
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

}  // namespace
}  // namespace bess::runtime
