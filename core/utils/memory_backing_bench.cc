// SPDX-License-Identifier: BSD-3-Clause

// What backing memory is worth to a lookup-heavy table (Decision D-029): the
// SlotTable shape -- an array of object pointers and the objects -- with
// random ids looked up in batches of 32 (independent loads, as a packet
// batch makes), over three backings of the same layout:
//
//   0  ordinary anonymous memory, 4 KiB pages (MADV_NOHUGEPAGE: BESS does
//      not rely on transparent hugepages, a host policy)
//   1  DPDK's heap (rte_malloc): whatever the EAL has -- hugepages (1 GiB
//      here) or, under --no-huge, normal pages
//
// Args: backing, entries (millions). Items = lookups.

#include <benchmark/benchmark.h>

#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#include "utils/dpdk_memory.h"

namespace {

struct Object {
  uint64_t value;
  uint64_t pad[3];  // 32 bytes, like a small action or next hop
};

struct Region {
  void *p = nullptr;
  size_t bytes = 0;
  bool dpdk = false;
  ~Region() {
    if (dpdk) {
      bess::utils::DpdkFree(p);
    } else if (p != nullptr) {
      munmap(p, bytes);
    }
  }
};

void Allocate(Region *r, size_t bytes, int backing) {
  r->bytes = bytes;
  if (backing == 1) {
    r->p = bess::utils::DpdkAllocate(bytes, 64);
    r->dpdk = true;
    return;
  }
  r->p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  madvise(r->p, bytes, MADV_NOHUGEPAGE);
}

void BM_TableLookup(benchmark::State &state) {
  const int backing = static_cast<int>(state.range(0));
  const size_t n = static_cast<size_t>(state.range(1)) << 20;
  Region slots_mem, objects_mem;
  Allocate(&slots_mem, n * sizeof(Object *), backing);
  Allocate(&objects_mem, n * sizeof(Object), backing);
  auto *slots = static_cast<Object **>(slots_mem.p);
  auto *objects = static_cast<Object *>(objects_mem.p);
  // Slot i points at a random object (published objects are scattered).
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(1);
  std::shuffle(order.begin(), order.end(), rng);
  for (size_t i = 0; i < n; i++) {
    objects[i].value = i;
    slots[i] = &objects[order[i]];
  }
  constexpr size_t kStream = 1 << 20;
  std::vector<uint32_t> ids(kStream);
  for (auto &id : ids) {
    id = static_cast<uint32_t>(rng() % n);
  }
  size_t at = 0;
  uint64_t sum = 0;
  for (auto _ : state) {
    for (size_t i = 0; i < 32; i++) {
      sum += slots[ids[at + i]]->value;
    }
    at = (at + 32) & (kStream - 1);
  }
  benchmark::DoNotOptimize(sum);
  state.SetItemsProcessed(state.iterations() * 32);
  static const char *kNames[] = {"4K-pages", "dpdk-heap"};
  state.SetLabel(kNames[backing]);
}
BENCHMARK(BM_TableLookup)->ArgsProduct({{0, 1}, {1, 4, 8}});

}  // namespace
