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
#include "runtime/malloc_policy.h"
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

// This thread's Headroom level (D-207).
thread_local Headroom::Level headroom = Headroom::kNone;

std::atomic<uint64_t> refusals{0};
std::atomic<uint64_t> spilled{0};

// AdmitTable's count of hugepage tables (D-198): admitted, not yet released.
std::mutex table_mu;
size_t hugepage_tables = 0;

// This thread's armed admission (D-206, ArmPlacement): the DPDK bytes it
// placed and not yet drawn by an allocation, by backing.
struct Armed {
  uint64_t ticket = 0;  // 0: unarmed
  size_t hugepage = 0;
  size_t normal = 0;
};
thread_local Armed armed;
std::atomic<PlacementObserver> placement_observer{nullptr};
std::atomic<bool> native_is_hugepages_for_test{false};

// DPDK's native heap is hugepages (detail::SetNativeHeapIsHugepagesForTest
// says so for test binaries, whose EAL runs on normal pages). The EAL must
// be up.
bool NativeHugepages() noexcept {
  return rte_eal_has_hugepages() ||
         native_is_hugepages_for_test.load(std::memory_order_relaxed);
}

// Draws `bytes` that landed `on` from this thread's arming: from the bytes
// placed there first, then from the other backing's, which move.
void Land(size_t bytes, TableBacking on) noexcept {
  if (armed.ticket == 0) {
    return;
  }
  size_t &same = on == TableBacking::kHugepages ? armed.hugepage : armed.normal;
  size_t &other = on == TableBacking::kHugepages ? armed.normal : armed.hugepage;
  const size_t drawn = std::min(bytes, same);
  same -= drawn;
  const size_t moved = std::min(bytes - drawn, other);
  if (moved == 0) {
    return;
  }
  other -= moved;
  {
    std::lock_guard<std::mutex> lock(table_mu);
    if (on == TableBacking::kHugepages) {
      hugepage_tables += moved;
    } else {
      hugepage_tables -= std::min(hugepage_tables, moved);
    }
  }
  if (on == TableBacking::kNormal) {
    spilled.fetch_add(moved, std::memory_order_relaxed);
  }
  if (PlacementObserver observer =
          placement_observer.load(std::memory_order_acquire)) {
    observer(armed.ticket, moved, on);
  }
}

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

// The bytes resident of [base, base + len), whole 4 KiB pages; 0 if mincore
// fails, which overstates what UnfaultedHeapBytes adds (refusing early, never
// late).
size_t ResidentBytes(const void *base, size_t len) {
  constexpr size_t kWindow = 16384;  // pages per mincore call
  unsigned char vec[kWindow];
  size_t pages = 0;
  for (size_t at = 0; at < len; at += kWindow * kPage) {
    const size_t n = std::min(len - at, kWindow * kPage);
    if (mincore(const_cast<char *>(static_cast<const char *>(base)) + at, n,
                vec) != 0) {
      return 0;
    }
    for (size_t i = 0; i < (n + kPage - 1) / kPage; i++) {
      pages += vec[i] & 1;
    }
  }
  return pages * kPage;
}

// What the normal-page DPDK heaps have allocated beyond what is resident
// (D-206): DPDK's rte_zmalloc leaves a clean element untouched and rte_free
// zeroes it, so a table a DPDK library allocates internally (rte_hash,
// rte_lpm, rte_fib) faults in as it is written or freed, not when it is
// allocated -- and a measurement alone would admit past bytes already
// handed out, which the teardown then faults in over the limit. Per heap of
// 4 KiB pages (DPDK's --no-huge heap; "bess-normal", which DPDK clears when
// a chunk is added), its allocated bytes (rte_malloc_get_socket_stats) less
// its resident ones (mincore over its memseg lists). Resident pages are
// already in the measurement: only the unfaulted part is added, nothing
// twice. DpdkFree gives a freed block's pages back; what a library frees
// (rte_free inside rte_hash_free) stays resident, and such free holes hide
// unfaulted allocations, a bound on how much it can miss. Control path;
// takes the memory hotplug lock, so never from a validator callback.
size_t UnfaultedHeapBytes() {
  if (!IsDpdkInitialized()) {
    return 0;
  }
  struct Heap {
    int socket;
    size_t resident;
  };
  std::vector<Heap> heaps;
  rte_memseg_list_walk(
      [](const rte_memseg_list *msl, void *arg) {
        if (msl->heap == 0 || msl->page_sz >= RTE_PGSIZE_2M ||
            msl->base_va == nullptr) {
          return 0;
        }
        auto &found = *static_cast<std::vector<Heap> *>(arg);
        const size_t resident = ResidentBytes(msl->base_va, msl->len);
        auto it = std::find_if(found.begin(), found.end(), [&](const Heap &h) {
          return h.socket == msl->socket_id;
        });
        if (it == found.end()) {
          found.push_back(Heap{msl->socket_id, resident});
        } else {
          it->resident += resident;
        }
        return 0;
      },
      &heaps);
  size_t unfaulted = 0;
  for (const Heap &heap : heaps) {
    rte_malloc_socket_stats stats{};
    if (rte_malloc_get_socket_stats(heap.socket, &stats) == 0 &&
        stats.heap_allocsz_bytes > heap.resident) {
      unfaulted += stats.heap_allocsz_bytes - heap.resident;
    }
  }
  return unfaulted;
}

[[noreturn]] void Refuse(MemoryRefused::Reason reason, size_t bytes,
                         std::string message) {
  refusals.fetch_add(1, std::memory_order_relaxed);
  LOG(WARNING) << message;
  throw MemoryRefused(reason, bytes, std::move(message));
}

// Makes a normal-page heap block resident now (D-206): DPDK zeroes a clean
// element when it is freed (malloc_elem_free), so a block admitted but never
// written would fault its whole size in at free. Faulted in at allocation,
// right after its admission, RSS matches what was admitted and a free never
// grows it. Hugepages are resident when mapped. "bess-normal" gives its free
// pages back (a new chunk's, and a freed block's: DpdkFree), so a block
// there is faulted in here too. What DPDK libraries allocate internally
// cannot be reached here; UnfaultedHeapBytes counts it instead.
void Prefault(void *p, size_t bytes) noexcept {
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23  // Linux 5.14
#endif
  const size_t head = reinterpret_cast<uintptr_t>(p) & (kPage - 1);
  if (madvise(static_cast<char *>(p) - head, head + bytes,
              MADV_POPULATE_WRITE) == 0) {
    return;
  }
  // Older kernels: write each page's first byte of the block back to itself.
  auto *bytes_at = static_cast<volatile unsigned char *>(p);
  for (size_t at = 0; at < bytes; at = at == 0 ? kPage - head : at + kPage) {
    bytes_at[at] = bytes_at[at];
  }
}

// BESS's heap lock (BP-3): DpdkFree gives a freed "bess-normal" block's pages
// back after rte_free returns, and an allocation in between could be handed
// those pages and lose what it wrote to them. Every allocation that can reach
// "bess-normal" holds it: BESS's own (HeapAllocate), and the DPDK library
// creates placed there (HeapGuard at CreateRteHash and the route tables'
// CreateOnPolicy). Nothing else allocates there: DPDK's own threads and
// SOCKET_ID_ANY never reach an external heap (D-206). Recursive, so a guarded
// call may allocate through HeapAllocate. Never take FallbackHeap's mutex
// while holding it (that one is taken first).
std::recursive_mutex heap_mu;
std::atomic<bool> giving_back{false};
std::atomic<uint64_t> heap_guards{0};
std::atomic<uint64_t> heap_guard_overlaps{0};

void *HeapAllocate(size_t bytes, size_t align, int socket) {
  const HeapGuard guard;
  return rte_malloc_socket("bess", bytes, static_cast<unsigned>(align), socket);
}

// Gives the 4 KiB pages of [p, p + bytes), free and zero to DPDK, back to the
// kernel (BP-3, U-1): a freed block stayed resident, and admission, which
// measures residency, counted it as used until a later block reused it -- a
// teardown and reload were refused. "bess-normal" is private anonymous
// memory: MADV_DONTNEED frees the pages, and they read back as zeroes, as
// DPDK expects of a clean free element. The caller holds heap_mu.
void ReturnPages(void *p, size_t bytes) noexcept {
  madvise(p, bytes, MADV_DONTNEED);
}

// The normal-page fallback heap (D-198): a DPDK external heap of anonymous
// 4 KiB pages, created on first use and grown by whole chunks, which DPDK
// commits as it adds them (malloc_heap_add_external_memory clears the
// region) -- so each chunk is admitted against the limit before it is
// mapped. The pages are first touched by the control thread adding them and
// so follow bessd's MPOL_PREFERRED memory node (D-123). Chunks are never
// unmapped, but their free pages go back to the kernel: a new chunk's once
// added, a freed block's in DpdkFree (BP-3). Control path only; the mutex
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
    void *p = HeapAllocate(bytes, align, socket_);
    if (p == nullptr) {
      // A new chunk is larger than bytes + alignment + slack: not reached.
      LOG(ERROR) << "fallback heap " << kFallbackHeap << " could not give "
                 << bytes << " bytes after growing";
      throw std::bad_alloc();
    }
    Prefault(p, bytes);
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
    // allocation needs, take just that. Unchecked prepaid memory (a removal
    // stage, Headroom::kPrepaid) takes only what it needs.
    size_t chunk = headroom == Headroom::kPrepaid
                       ? needed
                       : std::max({needed, kMinChunk,
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
    // DPDK clears the chunk as it adds it, so all of it is resident; what
    // no block uses goes back at once, but for the first and last pages
    // (the free element's header). Under heap_mu: the chunk is allocatable
    // as soon as it is added.
    const HeapGuard guard;
    if (rte_malloc_heap_memory_add(kFallbackHeap, va, chunk, nullptr,
                                   chunk / kPage, kPage) != 0) {
      LOG(ERROR) << "rte_malloc_heap_memory_add(" << kFallbackHeap << ", "
                 << Mb(chunk) << "): " << rte_strerror(rte_errno);
      munmap(va, chunk);
      throw std::bad_alloc();
    }
    ReturnPages(static_cast<char *>(va) + kPage, chunk - 2 * kPage);
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

namespace {

// AdmitNormalMemory's last measurement and what it has admitted since
// (D-206): class-C containers admit every node, so a measurement (two
// cgroupfs reads) per admission would cost more than the node. One is reused
// while the bytes admitted since stay under kRemeasureBytes, counted as if
// all were still held; a refusal is only ever made on a fresh measurement.
constexpr size_t kRemeasureBytes = kMB;
std::mutex measure_mu;
bool measured = false;
size_t measured_used = 0;
size_t admitted_since = 0;

// What usage may reach on this thread: the limit less the reserve, or less
// half of it under Headroom::kErase. Not called under kPrepaid.
size_t AllowedBytes(const MemoryPolicy &policy) noexcept {
  const size_t reserve = headroom == Headroom::kErase ? policy.reserve_bytes / 2
                                                      : policy.reserve_bytes;
  return policy.limit_bytes > reserve ? policy.limit_bytes - reserve : 0;
}

}  // namespace

Headroom::Headroom(Level level) noexcept : saved_(headroom) {
  headroom = std::max(saved_, level);
}

Headroom::~Headroom() { headroom = saved_; }

void SetMemoryPolicy(MemoryPolicy policy) {
  Policy() = std::move(policy);
  const std::lock_guard<std::mutex> lock(measure_mu);
  measured = false;
}

const MemoryPolicy &memory_policy() noexcept { return Policy(); }

void AdmitNormalMemory(size_t bytes) {
  const MemoryPolicy &policy = memory_policy();
  if (policy.limit_bytes == 0 || headroom == Headroom::kPrepaid) {
    return;
  }
  const size_t allowed = AllowedBytes(policy);
  {
    const std::lock_guard<std::mutex> lock(measure_mu);
    if (measured && bytes <= allowed &&
        admitted_since + bytes <= kRemeasureBytes &&
        measured_used + admitted_since <= allowed - bytes) {
      admitted_since += bytes;
      return;
    }
  }
  // Allocated DPDK heap memory counts whether or not it is resident yet: a
  // free faults it in (D-206).
  size_t used = UsedBytes(policy) + UnfaultedHeapBytes();
  if (bytes <= allowed && used > allowed - bytes) {
    // glibc keeps what was freed in its arenas (a transaction's decoded
    // copy, a request's buffers): given back (RT-1's trim) and measured
    // again, so free memory glibc retains never refuses an admission
    // (D-207). Only a would-be refusal pays for the trim.
    runtime::TrimAfterRelease(0);
    used = UsedBytes(policy) + UnfaultedHeapBytes();
  }
  {
    const std::lock_guard<std::mutex> lock(measure_mu);
    measured = true;
    measured_used = used;
    admitted_since = bytes;
  }
  if (bytes > allowed || used > allowed - bytes) {
    std::ostringstream message;
    message << "memory limit: refusing " << bytes << " bytes ("
            << Mb(bytes) << ") of normal-page memory: " << Mb(used)
            << " in use, the limit is " << Mb(policy.limit_bytes) << " ("
            << policy.limit_source << ") less a "
            << Mb(policy.limit_bytes - allowed) << " reserve";
    Refuse(MemoryRefused::Reason::kNormalLimit, bytes, message.str());
  }
}

namespace {

bool HugepageBacked() {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  return NativeHugepages();
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
  if (!policy.hugetlb_charged || policy.limit_bytes == 0 ||
      headroom == Headroom::kPrepaid) {
    return 0;
  }
  const size_t current = heap_bytes.load(std::memory_order_relaxed);
  const size_t growth = new_len > current ? new_len - current : 0;
  const size_t allowed = AllowedBytes(policy);
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

void SetNativeHeapIsHugepagesForTest(bool on) noexcept {
  native_is_hugepages_for_test.store(on, std::memory_order_relaxed);
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
            << ") already admitted, and normal pages past it are forbidden "
               "(--mem_fallback=false); raise the cap or allow them";
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
  if (NativeHugepages() && !memory_policy().fallback) {
    std::ostringstream message;
    message << "hugepages exhausted: refusing about " << bytes << " bytes ("
            << Mb(bytes) << ") of DPDK memory (" << HeapState(MemoryNode())
            << "), and normal pages past them are forbidden "
               "(--mem_fallback=false); give bessd more hugepages or allow "
               "normal pages";
    Refuse(MemoryRefused::Reason::kHugepagesExhausted, bytes, message.str());
  }
  return FallbackHeap::Get().Reserve(bytes);
}

void SetPlacementObserver(PlacementObserver observer) noexcept {
  placement_observer.store(observer, std::memory_order_release);
}

void ArmPlacement(uint64_t ticket, TableBacking backing, size_t bytes) noexcept {
  armed = Armed{ticket, backing == TableBacking::kHugepages ? bytes : 0,
                backing == TableBacking::kNormal ? bytes : 0};
}

bool ArmedForNormalPages() noexcept {
  return armed.ticket != 0 && armed.hugepage == 0 && armed.normal != 0 &&
         NativeHugepages();
}

int PlacementSocket(int socket, size_t bytes) {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  return ArmedForNormalPages() ? FallbackSocket(bytes) : socket;
}

void NotePlacement(size_t bytes, int socket) noexcept {
  const bool normal =
      !NativeHugepages() || socket == FallbackHeap::Get().socket();
  Land(bytes, normal ? TableBacking::kNormal : TableBacking::kHugepages);
}

void NotePlacement(size_t bytes, TableBacking landed) noexcept {
  Land(bytes, landed);
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
  status.spilled_bytes = spilled.load(std::memory_order_relaxed);
  if (!IsDpdkInitialized()) {
    return status;
  }
  status.hugepages = NativeHugepages();
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
  const bool hugepages = NativeHugepages();
  if (!hugepages) {
    // --no-huge (BP-3): tables come from "bess-normal", not DPDK's fixed
    // heap, so a freed one's pages can go back to the kernel (DpdkFree):
    // DPDK's own threads allocate from the fixed heap without BESS's heap
    // lock, never from an external heap. Tables are CPU memory: no IOVA
    // needed. Admitted first: this allocation is what may cross the limit
    // (D-198).
    AdmitNormalMemory(bytes);
    void *p = FallbackHeap::Get().Allocate(bytes, align);
    Land(bytes, TableBacking::kNormal);
    return p;
  }
  if (ArmedForNormalPages()) {
    // Admitted past the hugepage cap (D-206): normal pages, as admitted,
    // without taking hugepages the cap no longer has room for.
    void *p = FallbackHeap::Get().Allocate(bytes, align);
    Land(bytes, TableBacking::kNormal);
    return p;
  }
  limit_refused = false;
  void *p = HeapAllocate(bytes, align, socket);
  if (p == nullptr && socket != SOCKET_ID_ANY) {
    // The socket asked for is out of memory; any socket beats failing, but
    // the memory then crosses the interconnect on every access.
    static std::once_flag warned;
    std::call_once(warned, [socket] {
      LOG(WARNING) << "DPDK socket " << socket
                   << " is out of memory; allocating on any socket "
                      "(cross-NUMA memory; give that node more hugepages)";
    });
    p = HeapAllocate(bytes, align, SOCKET_ID_ANY);
  }
  if (p != nullptr) {
    if (rte_eal_has_hugepages() == 0) {
      Prefault(p, bytes);
    }
    Land(bytes, TableBacking::kHugepages);
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
  // DPDK's heap is out (D-198): normal pages unless the operator forbade
  // them (D-206).
  if (!memory_policy().fallback) {
    std::ostringstream message;
    message << "hugepages exhausted: refusing " << bytes << " bytes ("
            << Mb(bytes) << ") of DPDK memory (" << HeapState(socket)
            << "), and normal pages past them are forbidden "
               "(--mem_fallback=false); give bessd more hugepages or allow "
               "normal pages";
    Refuse(MemoryRefused::Reason::kHugepagesExhausted, bytes, message.str());
  }
  {
    static std::once_flag warned;
    std::call_once(warned, [] {
      LOG(WARNING) << "Hugepages exhausted: dataplane tables now also use the "
                      "normal-page fallback heap "
                   << kFallbackHeap << " (D-206)";
    });
  }
  p = FallbackHeap::Get().Allocate(bytes, align);
  Land(bytes, TableBacking::kNormal);
  return p;
}

void DpdkFree(void *p) noexcept {
  // The whole pages inside a block on "bess-normal" (its first and last may
  // hold neighbors' headers or data). Only there: every allocation from it
  // holds heap_mu, so none can be handed the pages between rte_free and
  // their return. DPDK's native heaps, which its own threads allocate from,
  // keep their pages (BP-3).
  size_t bytes = 0;
  const rte_memseg_list *msl = rte_mem_virt2memseg_list(p);
  const uintptr_t at = reinterpret_cast<uintptr_t>(p);
  const size_t head = RoundUp(at, kPage) - at;
  size_t pages = 0;
  if (msl != nullptr && msl->external != 0 && msl->heap != 0 &&
      msl->page_sz == kPage && rte_malloc_validate(p, &bytes) == 0 &&
      bytes > head) {
    pages = (bytes - head) / kPage;
  }
  if (pages == 0) {
    rte_free(p);
    return;
  }
  // From the memseg list, not from p, which rte_free ends.
  void *const first = static_cast<char *>(msl->base_va) +
                      (at + head - reinterpret_cast<uintptr_t>(msl->base_va));
  const HeapGuard guard;
  giving_back.store(true, std::memory_order_relaxed);
  rte_free(p);
  ReturnPages(first, pages * kPage);
  giving_back.store(false, std::memory_order_relaxed);
}

HeapGuard::HeapGuard() {
  heap_mu.lock();
  heap_guards.fetch_add(1, std::memory_order_relaxed);
  if (giving_back.load(std::memory_order_relaxed)) {
    heap_guard_overlaps.fetch_add(1, std::memory_order_relaxed);
  }
}

HeapGuard::~HeapGuard() {
  heap_mu.unlock();
}

namespace detail {

uint64_t HeapGuards() noexcept {
  return heap_guards.load(std::memory_order_relaxed);
}

uint64_t HeapGuardOverlaps() noexcept {
  return heap_guard_overlaps.load(std::memory_order_relaxed);
}

}  // namespace detail

}  // namespace bess::utils
