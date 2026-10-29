// SPDX-License-Identifier: BSD-3-Clause

#include "utils/dpdk_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_malloc.h>

#include "dpdk.h"
#include "runtime/numa_placement.h"
#include "utils/logging.h"

namespace bess::utils {

namespace {

constexpr size_t kMB = size_t{1} << 20;
constexpr size_t kPage = 4096;  // the fallback heap's page size
constexpr const char *kFallbackHeap = "bess-normal";
// Fallback heap growth (D-198): chunks of at least 64 MiB, each twice the
// previous one up to 1 GiB, so a large table costs a few memseg lists (DPDK
// has 128 in all) and a small one does not commit a gigabyte.
constexpr size_t kMinChunk = 64 * kMB;
constexpr size_t kMaxDoubling = 1024 * kMB;
// Headroom over the bytes asked for in a new chunk: the malloc element
// header and trailer, alignment padding, and the heap's own bookkeeping at
// the start of the segment. Generous on purpose; it is 64 KiB.
constexpr size_t kChunkSlack = size_t{64} * 1024;

std::atomic<uint64_t> refusals{0};

// AdmitTable's count of hugepage tables (D-198): admitted, not yet released.
std::mutex table_mu;
size_t hugepage_tables = 0;

MemoryPolicy &Policy() {
  static MemoryPolicy policy;
  return policy;
}

size_t RoundUp(size_t n, size_t to) { return (n + to - 1) / to * to; }

std::string Mb(size_t bytes) {
  return std::to_string(bytes / kMB) + " MB";
}

bool ReadUint64(const std::string &path, uint64_t *value) {
  std::ifstream in(path);
  return static_cast<bool>(in >> *value);
}

// The working set the limit is compared against (D-198): the cgroup's
// memory.current less its inactive page cache (memory.stat inactive_file),
// which the kernel reclaims before it OOM-kills anyone; without a cgroup
// directory (or if its files cannot be read), process RSS plus charged
// hugetlb pages (Linux excludes those from RSS). Control path only.
size_t UsedBytes(const MemoryPolicy &policy) {
  if (!policy.cgroup_dir.empty()) {
    uint64_t current = 0;
    if (ReadUint64(policy.cgroup_dir + "/memory.current", &current)) {
      std::ifstream stat(policy.cgroup_dir + "/memory.stat");
      std::string key;
      uint64_t value = 0;
      while (stat >> key >> value) {
        if (key == "inactive_file") {
          return current - std::min(current, value);
        }
      }
      return current;
    }
  }
  std::ifstream statm("/proc/self/statm");
  uint64_t size_pages = 0, resident_pages = 0;
  size_t used = 0;
  if (statm >> size_pages >> resident_pages) {
    used = resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  }
  if (policy.hugetlb_charged) {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
      if (key == "HugetlbPages:") {
        uint64_t kb = 0;
        if (status >> kb) used += kb * 1024;
        break;
      }
      status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
  }
  return used;
}

[[noreturn]] void Refuse(MemoryRefused::Reason reason, size_t bytes,
                         std::string message) {
  refusals.fetch_add(1, std::memory_order_relaxed);
  LOG(WARNING) << message;
  throw MemoryRefused(reason, bytes, std::move(message));
}

// The normal-page fallback heap (D-198): a DPDK external heap of anonymous
// 4 KiB pages, created on first use and grown by whole chunks, which DPDK
// commits as it adds them (malloc_heap_add_external_memory clears the
// region) -- so each chunk is admitted against the limit before it is
// mapped. The pages are first touched by the control thread adding them and
// so follow bessd's MPOL_PREFERRED memory node (D-123). Chunks are never
// unmapped: freed memory goes back to the heap. Control path only; the mutex
// orders creation and growth.
class FallbackHeap {
 public:
  static FallbackHeap &Get() {
    static FallbackHeap heap;
    return heap;
  }

  // Allocates from the heap, growing it if it has no free block that fits.
  void *Allocate(size_t bytes, size_t align) {
    std::lock_guard<std::mutex> lock(mu_);
    EnsureLocked(bytes, align);
    void *p = rte_malloc_socket("bess", bytes, static_cast<unsigned>(align),
                                socket_);
    if (p == nullptr) {
      // A new chunk is larger than bytes + alignment + slack: not reached.
      LOG(ERROR) << "fallback heap " << kFallbackHeap << " could not give "
                 << bytes << " bytes after growing";
      throw std::bad_alloc();
    }
    return p;
  }

  // The heap's socket id, with at least `bytes` free in one block.
  int Reserve(size_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    EnsureLocked(bytes, RTE_CACHE_LINE_SIZE);
    return socket_;
  }

  // -1 before the heap exists.
  int socket() {
    std::lock_guard<std::mutex> lock(mu_);
    return socket_;
  }

 private:
  void EnsureLocked(size_t bytes, size_t align) {
    if (socket_ < 0) {
      if (rte_malloc_heap_create(kFallbackHeap) != 0 && rte_errno != EEXIST) {
        LOG(ERROR) << "rte_malloc_heap_create(" << kFallbackHeap
                   << "): " << rte_strerror(rte_errno);
        throw std::bad_alloc();
      }
      socket_ = rte_malloc_heap_get_socket(kFallbackHeap);
      if (socket_ < 0) {
        LOG(ERROR) << "rte_malloc_heap_get_socket(" << kFallbackHeap
                   << "): " << rte_strerror(rte_errno);
        throw std::bad_alloc();
      }
    }
    const size_t needed = RoundUp(bytes + align + kChunkSlack, kPage);
    rte_malloc_socket_stats stats{};
    if (rte_malloc_get_socket_stats(socket_, &stats) == 0 &&
        stats.greatest_free_size >= bytes + align + kChunkSlack) {
      return;
    }
    // Prefer a large chunk; if the limit leaves room only for what this
    // allocation needs, take just that.
    size_t chunk = std::max({needed, kMinChunk,
                             std::min(last_chunk_ * 2, kMaxDoubling)});
    try {
      AdmitNormalMemory(chunk);
    } catch (const MemoryRefused &) {
      if (chunk == needed) {
        throw;
      }
      chunk = needed;
      AdmitNormalMemory(chunk);
    }
    void *va = mmap(nullptr, chunk, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (va == MAP_FAILED) {
      PLOG(ERROR) << "mmap of " << Mb(chunk) << " for " << kFallbackHeap;
      throw std::bad_alloc();
    }
    if (rte_malloc_heap_memory_add(kFallbackHeap, va, chunk, nullptr,
                                   chunk / kPage, kPage) != 0) {
      LOG(ERROR) << "rte_malloc_heap_memory_add(" << kFallbackHeap << ", "
                 << Mb(chunk) << "): " << rte_strerror(rte_errno);
      munmap(va, chunk);
      throw std::bad_alloc();
    }
    last_chunk_ = chunk;
    LOG(INFO) << "Normal-page fallback heap " << kFallbackHeap << " grew by "
              << Mb(chunk) << " (" << bytes << " bytes asked for)";
  }

  std::mutex mu_;
  int socket_ = -1;
  size_t last_chunk_ = 0;
};

// The memory node's native heap, for refusal messages.
std::string HeapState(int socket) {
  if (socket == SOCKET_ID_ANY) {
    socket = MemoryNode();
  }
  rte_malloc_socket_stats stats{};
  if (rte_malloc_get_socket_stats(socket, &stats) != 0) {
    return "socket " + std::to_string(socket) + ": no heap";
  }
  return "socket " + std::to_string(socket) + " heap " +
         Mb(stats.heap_totalsz_bytes) + ", " + Mb(stats.heap_allocsz_bytes) +
         " allocated, largest free block " + Mb(stats.greatest_free_size);
}

}  // namespace

MemoryRefused::MemoryRefused(Reason reason, size_t bytes, std::string message)
    : reason_(reason),
      bytes_(bytes),
      message_(std::make_shared<const std::string>(std::move(message))) {}

const char *MemoryRefused::what() const noexcept { return message_->c_str(); }

void SetMemoryPolicy(MemoryPolicy policy) { Policy() = std::move(policy); }

const MemoryPolicy &memory_policy() noexcept { return Policy(); }

void AdmitNormalMemory(size_t bytes) {
  const MemoryPolicy &policy = memory_policy();
  if (policy.limit_bytes == 0) {
    return;
  }
  const size_t allowed = policy.limit_bytes > policy.reserve_bytes
                             ? policy.limit_bytes - policy.reserve_bytes
                             : 0;
  const size_t used = UsedBytes(policy);
  if (bytes > allowed || used > allowed - bytes) {
    std::ostringstream message;
    message << "memory limit: refusing " << bytes << " bytes ("
            << Mb(bytes) << ") of normal-page memory: " << Mb(used)
            << " in use, the limit is " << Mb(policy.limit_bytes) << " ("
            << policy.limit_source << ") less a " << Mb(policy.reserve_bytes)
            << " reserve";
    Refuse(MemoryRefused::Reason::kNormalLimit, bytes, message.str());
  }
}

namespace {

bool HugepageBacked() {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  return rte_eal_has_hugepages();
}

// The page size of the memory node's hugepage heap: the first memory
// segment list DPDK made for that socket (bessd maps one page size per
// node). Read once, on the control path, never from a validator callback
// (rte_memseg_list_walk takes the hotplug lock those run under).
size_t HeapPageSize() {
  static const size_t page = [] {
    struct Find {
      int socket;
      size_t page;
    } find{MemoryNode(), 0};
    rte_memseg_list_walk(
        [](const rte_memseg_list *msl, void *arg) {
          auto *f = static_cast<Find *>(arg);
          if (msl->external || msl->socket_id != f->socket) {
            return 0;
          }
          f->page = msl->page_sz;
          return 1;
        },
        &find);
    return find.page;
  }();
  return page;
}

// The memory node heap's size as DPDK grows and shrinks it (memory events),
// for the validator, which is told only the size the heap would reach.
std::atomic<size_t> heap_bytes{0};
int validated_socket = -1;
// Set by the validator when it refuses this thread's growth for the memory
// limit, read by DpdkAllocate to say so instead of "hugepages exhausted".
thread_local bool limit_refused = false;

void OnHeapEvent(rte_mem_event event, const void *addr, size_t len, void *) {
  const rte_memseg_list *msl = rte_mem_virt2memseg_list(addr);
  if (msl == nullptr || msl->external || msl->socket_id != validated_socket) {
    return;
  }
  if (event == RTE_MEM_EVENT_ALLOC) {
    heap_bytes.fetch_add(len, std::memory_order_relaxed);
  } else {
    heap_bytes.fetch_sub(std::min(len, heap_bytes.load()), std::memory_order_relaxed);
  }
}

// Runs with DPDK's memory hotplug lock held, before the heap maps pages:
// no DPDK memory calls here. -1 refuses the growth (rte_malloc fails).
int ValidateHeapGrowth(int, size_t, size_t new_len) {
  const MemoryPolicy &policy = memory_policy();
  if (policy.hugepage_cap_bytes != 0 && new_len > policy.hugepage_cap_bytes) {
    refusals.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  if (!policy.hugetlb_charged || policy.limit_bytes == 0) {
    return 0;
  }
  const size_t current = heap_bytes.load(std::memory_order_relaxed);
  const size_t growth = new_len > current ? new_len - current : 0;
  const size_t allowed = policy.limit_bytes > policy.reserve_bytes
                             ? policy.limit_bytes - policy.reserve_bytes
                             : 0;
  const size_t used = UsedBytes(policy);
  if (growth > allowed || used > allowed - growth) {
    refusals.fetch_add(1, std::memory_order_relaxed);
    limit_refused = true;
    return -1;
  }
  return 0;
}

}  // namespace

namespace detail {

size_t HugepageCharge(size_t bytes, size_t largest_free, size_t page_size) {
  if (bytes <= largest_free) {
    return 0;  // the heap holds it in pages it has already mapped
  }
  return page_size == 0 ? bytes : RoundUp(bytes, page_size);
}

}  // namespace detail

void InstallHeapValidator() {
  if (!IsDpdkInitialized() || !rte_eal_has_hugepages()) {
    return;
  }
  static std::once_flag once;
  std::call_once(once, [] {
    validated_socket = MemoryNode();
    rte_malloc_socket_stats stats{};
    if (rte_malloc_get_socket_stats(validated_socket, &stats) == 0) {
      heap_bytes.store(stats.heap_totalsz_bytes, std::memory_order_relaxed);
    }
    // Limit 1: called for every growth (DPDK skips a validator whose limit
    // is above the size the heap would reach).
    if (rte_mem_event_callback_register("bess-heap", OnHeapEvent, nullptr) != 0 ||
        rte_mem_alloc_validator_register("bess-limit", ValidateHeapGrowth,
                                         validated_socket, 1) != 0) {
      LOG(WARNING) << "DPDK memory validator unavailable ("
                   << rte_strerror(rte_errno)
                   << "): hugepage heap growth is not checked against the "
                      "memory limit (D-198)";
    }
  });
}

TableBacking AdmitTable(size_t bytes, size_t glibc_bytes) {
  glibc_bytes = std::min(glibc_bytes, bytes);
  if (!HugepageBacked()) {
    AdmitNormalMemory(bytes);
    return TableBacking::kNormal;
  }
  const MemoryPolicy &policy = memory_policy();
  const size_t dpdk_bytes = bytes - glibc_bytes;
  bool counted = false;
  {
    std::lock_guard<std::mutex> lock(table_mu);
    const size_t cap = policy.hugepage_cap_bytes;
    if (cap == 0 ||
        (dpdk_bytes <= cap && hugepage_tables <= cap - dpdk_bytes)) {
      hugepage_tables += dpdk_bytes;
      counted = true;
    }
  }
  if (counted) {
    // Charged hugepages count against memory.max as their pages are mapped
    // (D-198): what the heap must add for this table is admitted with the
    // glibc part.
    size_t charge = 0;
    if (policy.hugetlb_charged && dpdk_bytes != 0) {
      rte_malloc_socket_stats stats{};
      const size_t largest_free =
          rte_malloc_get_socket_stats(MemoryNode(), &stats) == 0
              ? stats.greatest_free_size
              : 0;
      charge = detail::HugepageCharge(dpdk_bytes, largest_free, HeapPageSize());
    }
    if (glibc_bytes + charge == 0) {
      return TableBacking::kHugepages;
    }
    try {
      AdmitNormalMemory(glibc_bytes + charge);
    } catch (...) {
      ReleaseTable(TableBacking::kHugepages, dpdk_bytes);
      throw;
    }
    return TableBacking::kHugepages;
  }
  if (!policy.fallback) {
    std::ostringstream message;
    message << "hugepages exhausted: refusing " << dpdk_bytes << " bytes ("
            << Mb(dpdk_bytes) << ") for a table: "
            << Mb(HugepageTableBytes()) << " of the "
            << Mb(policy.hugepage_cap_bytes) << " hugepage cap ("
            << policy.hugepage_cap_source
            << ") already admitted; raise the cap or start bessd with "
               "--mem_fallback to use normal pages instead";
    Refuse(MemoryRefused::Reason::kHugepagesExhausted, dpdk_bytes,
           message.str());
  }
  AdmitNormalMemory(bytes);
  return TableBacking::kNormal;
}

void ReleaseTable(TableBacking backing, size_t bytes) noexcept {
  if (backing != TableBacking::kHugepages) {
    return;  // normal pages are measured, not counted
  }
  std::lock_guard<std::mutex> lock(table_mu);
  hugepage_tables -= std::min(hugepage_tables, bytes);
}

size_t HugepageTableBytes() noexcept {
  std::lock_guard<std::mutex> lock(table_mu);
  return hugepage_tables;
}

int FallbackSocket(size_t bytes) {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  if (rte_eal_has_hugepages() && !memory_policy().fallback) {
    std::ostringstream message;
    message << "hugepages exhausted: refusing about " << bytes << " bytes ("
            << Mb(bytes) << ") of DPDK memory (" << HeapState(MemoryNode())
            << "); give bessd more hugepages or start it with "
               "--mem_fallback to use normal pages instead";
    Refuse(MemoryRefused::Reason::kHugepagesExhausted, bytes, message.str());
  }
  return FallbackHeap::Get().Reserve(bytes);
}

void RefuseDpdkMemory(size_t bytes) {
  std::ostringstream message;
  if (HugepageBacked()) {
    message << "hugepages exhausted: refusing about " << bytes << " bytes ("
            << Mb(bytes) << ") of DPDK memory (" << HeapState(MemoryNode())
            << "); give bessd more hugepages";
    Refuse(MemoryRefused::Reason::kHugepagesExhausted, bytes, message.str());
  }
  message << "memory limit: refusing about " << bytes << " bytes ("
          << Mb(bytes) << ") of normal-page memory: DPDK's --no-huge heap is "
          << "full (" << HeapState(MemoryNode())
          << "); raise the memory limit (--mem_limit_mb), which sizes it";
  Refuse(MemoryRefused::Reason::kNormalLimit, bytes, message.str());
}

MemoryStatus GetMemoryStatus() {
  const MemoryPolicy &policy = memory_policy();
  MemoryStatus status;
  status.fallback = policy.fallback;
  status.hugetlb_charged = policy.hugetlb_charged;
  status.hugepage_cap_bytes = policy.hugepage_cap_bytes;
  status.hugepage_cap_source = policy.hugepage_cap_source;
  status.hugepage_table_bytes = HugepageTableBytes();
  status.limit_bytes = policy.limit_bytes;
  status.reserve_bytes = policy.reserve_bytes;
  status.limit_source = policy.limit_source;
  status.used_bytes = UsedBytes(policy);
  status.refusals = refusals.load(std::memory_order_relaxed);
  if (!IsDpdkInitialized()) {
    return status;
  }
  status.hugepages = rte_eal_has_hugepages();
  rte_malloc_socket_stats stats{};
  // External heaps are not among rte_socket_count()'s sockets.
  for (unsigned i = 0; i < rte_socket_count(); i++) {
    if (rte_malloc_get_socket_stats(rte_socket_id_by_idx(i), &stats) == 0) {
      status.dpdk_heap_bytes += stats.heap_totalsz_bytes;
      status.dpdk_heap_alloc_bytes += stats.heap_allocsz_bytes;
    }
  }
  const int fallback = FallbackHeap::Get().socket();
  if (fallback >= 0 && rte_malloc_get_socket_stats(fallback, &stats) == 0) {
    status.fallback_heap_bytes = stats.heap_totalsz_bytes;
    status.fallback_heap_alloc_bytes = stats.heap_allocsz_bytes;
  }
  return status;
}

int MemoryNode() {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  // --no-huge: DPDK's one normal-page heap is labelled socket 0 whatever node
  // its pages are on (they follow the process mempolicy, the memory node).
  return rte_eal_has_hugepages() ? bess::runtime::MemoryNode() : 0;
}

void *DpdkAllocate(size_t bytes, size_t align, int socket) {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  if (bytes == 0) {
    bytes = 1;
  }
  const bool hugepages = rte_eal_has_hugepages();
  if (!hugepages) {
    // DPDK's fixed --no-huge heap is reserved, not resident: its pages are
    // committed as they are first written, so this allocation is what may
    // cross the limit (D-198).
    AdmitNormalMemory(bytes);
  }
  limit_refused = false;
  void *p = rte_malloc_socket("bess", bytes, static_cast<unsigned>(align),
                              socket);
  if (p == nullptr && socket != SOCKET_ID_ANY) {
    // The socket asked for is out of memory; any socket beats failing, but
    // the memory then crosses the interconnect on every access.
    static std::once_flag warned;
    std::call_once(warned, [socket] {
      LOG(WARNING) << "DPDK socket " << socket
                   << " is out of memory; allocating on any socket "
                      "(cross-NUMA memory; give that node more hugepages)";
    });
    p = rte_malloc_socket("bess", bytes, static_cast<unsigned>(align),
                          SOCKET_ID_ANY);
  }
  if (p != nullptr) {
    return p;
  }
  if (limit_refused) {
    // The heap validator refused the pages (D-198): hugepages are charged to
    // memory.max here, and the fallback heap's normal pages would be charged
    // to the same limit, so there is nothing to fall back to.
    const MemoryPolicy &policy = memory_policy();
    std::ostringstream message;
    message << "memory limit: refusing " << bytes << " bytes (" << Mb(bytes)
            << ") of hugepage memory: hugepages are charged to memory.max "
               "here (cgroup2 memory_hugetlb_accounting) and the heap's next "
               "pages would cross the "
            << Mb(policy.limit_bytes) << " limit (" << policy.limit_source
            << ") less a " << Mb(policy.reserve_bytes)
            << " reserve; raise the memory limit to include the hugepages";
    Refuse(MemoryRefused::Reason::kNormalLimit, bytes, message.str());
  }
  // DPDK's heap is out (D-198). With hugepages, normal pages only when the
  // operator declared them acceptable; under --no-huge the fixed heap is
  // normal pages already, and the fallback heap merely extends it.
  if (hugepages && !memory_policy().fallback) {
    std::ostringstream message;
    message << "hugepages exhausted: refusing " << bytes << " bytes ("
            << Mb(bytes) << ") of DPDK memory (" << HeapState(socket)
            << "); give bessd more hugepages or start it with "
               "--mem_fallback to use normal pages instead";
    Refuse(MemoryRefused::Reason::kHugepagesExhausted, bytes, message.str());
  }
  {
    static std::once_flag warned;
    std::call_once(warned, [hugepages] {
      LOG(WARNING) << (hugepages ? "Hugepages exhausted"
                                 : "DPDK's --no-huge heap is full")
                   << ": dataplane tables now also use the normal-page "
                      "fallback heap "
                   << kFallbackHeap << " (D-198)";
    });
  }
  return FallbackHeap::Get().Allocate(bytes, align);
}

void DpdkFree(void *p) noexcept { rte_free(p); }

}  // namespace bess::utils
