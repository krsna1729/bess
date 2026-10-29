// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_DPDK_MEMORY_H_
#define BESS_UTILS_DPDK_MEMORY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include <rte_memory.h>

namespace bess::utils {

// The DPDK socket id of the process memory node (Decision D-123): bessd picks
// one NUMA node at startup and everything it allocates lives there. With
// hugepages it is bess::runtime::MemoryNode(); under --no-huge DPDK labels its
// whole heap socket 0 (the pages physically follow the process mempolicy,
// which bessd sets to the memory node), so it is 0 there -- asking for any
// other socket would fail. Pass it wherever DPDK takes a socket id; NUMA
// classification of CPUs and devices uses the raw bess::runtime::MemoryNode().
//
// Initializes DPDK first if nothing has yet (tests, benchmarks).
int MemoryNode();

// Dataplane memory policy (Decision D-198, "Dataplane memory: hugepage-first
// with a declared fallback and a limit"): every N-scaled table a worker reads
// comes from DPDK's heap (D-029) on the memory node (D-123). When hugepages
// run out a creation is refused unless bessd was started with --mem_fallback,
// which lets it take normal (4 KiB) pages instead -- slower lookups, declared
// rather than silent. Normal-page memory is admitted against the process's
// memory limit (cgroup v2 memory.max, --mem_limit_mb) first, so a creation or
// transaction that would cross it is refused with a clear, non-retryable error
// instead of the kernel OOM-killing bessd. Everything here runs on the control
// path; nothing on the packet path changes.

// Thrown on the control path when memory cannot be given. Derives from
// std::bad_alloc so existing bad_alloc handling still applies.
class MemoryRefused : public std::bad_alloc {
 public:
  enum class Reason : uint8_t { kHugepagesExhausted, kNormalLimit };
  MemoryRefused(Reason reason, size_t bytes, std::string message);
  const char *what() const noexcept override;  // the message
  Reason reason() const noexcept { return reason_; }
  size_t bytes() const noexcept { return bytes_; }

 private:
  Reason reason_;
  size_t bytes_;
  // Shared, so copying the exception (as throw and catch may) cannot throw.
  std::shared_ptr<const std::string> message_;
};

// Which pages the DPDK heap is built from (--mem_backing). kAuto: hugepages
// if any are usable at startup, else normal pages; bessd resolves it before
// the EAL starts, so after startup the EAL itself says which one it got
// (rte_eal_has_hugepages()).
enum class MemoryBacking : uint8_t { kAuto, kHugepages, kNormal };

struct MemoryPolicy {
  MemoryBacking backing = MemoryBacking::kAuto;  // --mem_backing
  // --mem_hugepage_mb, or the cgroup's hugetlb.<size>.max when smaller (or
  // the flag is 0): the hugepage heap on the memory node is capped here
  // (DPDK --socket-limit); table admission counts against it. 0: no cap.
  size_t hugepage_cap_bytes = 0;
  std::string hugepage_cap_source;  // "--mem_hugepage_mb", "cgroup hugetlb..."
  // Hugepages are charged to memory.max (cgroup2 memory_hugetlb_accounting):
  // hugepage growth is admitted against the memory limit too.
  bool hugetlb_charged = false;
  bool fallback = false;     // --mem_fallback
  size_t limit_bytes = 0;    // --mem_limit_mb (resolved); 0: no limit known
  size_t reserve_bytes = 0;  // --mem_reserve_mb (resolved): kept free for
                             // unaccounted (glibc, control) memory
  // e.g. "cgroup memory.max (/user.slice/x.scope)", "--mem_limit_mb",
  // "MemTotal"
  std::string limit_source;
  // Absolute cgroupfs directory whose memory.current and memory.stat measure
  // usage against the limit; empty: RSS plus hugetlb when it is charged.
  std::string cgroup_dir;
};
// bessd sets it at startup, before the first pool or table; it is not
// synchronized with readers. Until then it is default-constructed (tests,
// library users): no limit, no fallback under hugepages.
void SetMemoryPolicy(MemoryPolicy policy);
const MemoryPolicy &memory_policy() noexcept;

// Normal-page memory about to be committed. Usage is the cgroup's working set
// (memory.current less memory.stat inactive_file: reclaimable page cache is
// not what the OOM killer counts against bessd) when cgroup_dir is set, else
// process RSS plus hugetlb when charged. Throws MemoryRefused(kNormalLimit) if usage + bytes would
// exceed limit - reserve. No-op without a limit.
void AdmitNormalMemory(size_t bytes);

// Where an admitted table's DPDK-heap bytes are expected to land
// (AdmitTable).
enum class TableBacking : uint8_t { kHugepages, kNormal };

// The admission for a table or other sizeable dataplane primitive, before
// it is allocated (framework::MemoryScope calls it; D-198). `bytes` is the
// whole cost; `glibc_bytes` the part of it a primitive takes from glibc
// (normal pages whatever DPDK runs on: the flow tables' slots, timer wheels,
// rings from AllocRingMem), the rest comes from DPDK's heap. Normal-page
// backing: AdmitNormalMemory(bytes). Hugepage backing: the glibc part is
// admitted against the limit, and the DPDK part is admitted and counted
// while the hugepage tables admitted so far plus it stay within
// hugepage_cap_bytes (0: no cap) -- and, when hugetlb is charged to
// memory.max, the whole pages the heap must add for it (HugepageCharge) are
// admitted against the limit as well; past the cap, AdmitNormalMemory(bytes)
// when --mem_fallback was given (the table will come from the fallback
// heap), else MemoryRefused(kHugepagesExhausted). Returns where the DPDK
// part goes. Throws MemoryRefused and counts nothing on refusal. Control
// path; thread-safe.
TableBacking AdmitTable(size_t bytes, size_t glibc_bytes = 0);
// Gives back the DPDK part (bytes - glibc_bytes) AdmitTable admitted, with
// the backing it returned; normal pages are measured, not counted.
void ReleaseTable(TableBacking backing, size_t bytes) noexcept;
// The hugepage bytes AdmitTable has counted and not yet released.
size_t HugepageTableBytes() noexcept;

namespace detail {
// What a hugepage table of `bytes` adds to memory.max when hugetlb is
// charged (D-198): nothing if the heap's largest free block holds it, else
// the whole pages the heap must map for it (DPDK grows the heap by pages).
size_t HugepageCharge(size_t bytes, size_t largest_free, size_t page_size);
}  // namespace detail

// The hugepage heap's growth gate (D-198): a DPDK allocation validator on
// the memory node, called before the heap maps more pages, which refuses
// growth past the hugepage cap and, when hugetlb is charged to memory.max,
// growth the memory limit cannot take -- rte_malloc then returns NULL and
// the caller's refusal (or fallback) runs, instead of a page faulted past
// memory.max killing bessd with SIGBUS. Installed once after the EAL comes
// up with hugepages (runtime/dpdk.cc); a no-op otherwise.
void InstallHeapValidator();

// The admission a primitive that grows after construction asks before it
// allocates more (D-198): the owner hands it one (framework::MemoryLease's
// growth(); dataplane libraries cannot see the framework, so the hook lives
// here), and the primitive calls Grow with the bytes it is about to add
// (`glibc_bytes` of them from glibc, as in AdmitTable) before it allocates
// them, and Shrink with what it gave back after. Grow throws MemoryRefused
// to refuse the growth, which then must not happen. Without a hook a
// primitive grows unadmitted (tests, library users). Control path only.
class MemoryGrowth {
 public:
  virtual void Grow(size_t bytes, size_t glibc_bytes = 0) = 0;
  virtual void Shrink(size_t bytes, size_t glibc_bytes = 0) noexcept = 0;

 protected:
  ~MemoryGrowth() = default;
};

// For DPDK libraries that allocate internally (rte_hash, rte_lpm, rte_fib,
// rte_ring): call after creating on MemoryNode() failed for lack of memory,
// with an estimate of the bytes the object needs. Returns the socket id of
// the normal-page fallback heap (a DPDK external heap "bess-normal", grown
// and admitted as needed, so at least `bytes` are free in one piece), or
// throws MemoryRefused(kHugepagesExhausted) when fallback is not allowed.
int FallbackSocket(size_t bytes);

// For a DPDK library allocation that failed for lack of memory where no
// fallback applies (rte_hash's RCU defer queue and tailq entries, which DPDK
// takes from its native heaps, never the fallback heap): throws
// MemoryRefused for about `bytes` -- kHugepagesExhausted with hugepages,
// kNormalLimit when DPDK's fixed --no-huge heap is full.
[[noreturn]] void RefuseDpdkMemory(size_t bytes);

struct MemoryStatus {
  bool hugepages = false;
  bool fallback = false;
  bool hugetlb_charged = false;
  size_t hugepage_cap_bytes = 0;
  std::string hugepage_cap_source;
  size_t hugepage_table_bytes = 0;  // AdmitTable's hugepage count
  size_t limit_bytes = 0, reserve_bytes = 0, used_bytes = 0;
  std::string limit_source;
  size_t dpdk_heap_bytes = 0, dpdk_heap_alloc_bytes = 0;  // native heap(s)
  size_t fallback_heap_bytes = 0, fallback_heap_alloc_bytes = 0;  // "bess-normal"
  uint64_t refusals = 0;  // MemoryRefused thrown so far
};
MemoryStatus GetMemoryStatus();

// Performance-critical dataplane memory comes from DPDK's heap (Decision
// D-029): hugepage-backed when bessd has hugepages, DPDK's normal-page heap
// under --no-huge -- whatever the EAL was initialized with, and one
// allocator rather than a parallel one. On the NUMA socket asked for, by
// default the memory node (SOCKET_ID_ANY: DPDK's choice, the caller's socket
// first); a socket out of memory falls back to any socket, warned once.
// Allocation happens at creation, on the control path; running out is an
// error there, never a packet-path event (D-198): under --no-huge the bytes
// are admitted against the memory limit first; hugepages exhausted means the
// normal-page fallback heap with --mem_fallback (always under --no-huge,
// whose fixed heap may fill), else MemoryRefused.
//
// Initializes DPDK first if nothing has yet (tests, benchmarks).
void *DpdkAllocate(size_t bytes, size_t align, int socket = MemoryNode());
// rte_free, for the native and the fallback heap alike.
void DpdkFree(void *p) noexcept;

template <typename T>
struct DpdkDelete {
  void operator()(T *p) const noexcept {
    if (p != nullptr) {
      p->~T();
      DpdkFree(const_cast<std::remove_const_t<T> *>(p));
    }
  }
};

// A single object in DPDK memory.
template <typename T>
using DpdkUnique = std::unique_ptr<T, DpdkDelete<T>>;

template <typename T, typename... Args>
DpdkUnique<T> MakeDpdk(int socket, Args &&...args) {
  void *p = DpdkAllocate(sizeof(T), alignof(T), socket);
  try {
    return DpdkUnique<T>(new (p) T(std::forward<Args>(args)...));
  } catch (...) {
    DpdkFree(p);
    throw;
  }
}

// A fixed-size array of value-initialized T in DPDK memory, cache-line
// aligned (or T's alignment, if larger).
template <typename T>
class DpdkArray {
 public:
  // What DpdkArray(n, socket) allocates (D-198: framework::MemoryCost).
  static size_t Bytes(size_t n, int /*socket*/ = 0) noexcept { return n * sizeof(T); }

  DpdkArray() = default;
  DpdkArray(size_t n, int socket = MemoryNode()) : size_(n) {
    constexpr size_t kAlign =
        alignof(T) > RTE_CACHE_LINE_SIZE ? alignof(T) : RTE_CACHE_LINE_SIZE;
    data_ = static_cast<T *>(DpdkAllocate(n * sizeof(T), kAlign, socket));
    size_t built = 0;
    try {
      for (; built < n; built++) {
        new (&data_[built]) T();
      }
    } catch (...) {
      Destroy(built);
      throw;
    }
  }
  ~DpdkArray() { Destroy(size_); }
  DpdkArray(DpdkArray &&o) noexcept
      : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}
  DpdkArray &operator=(DpdkArray &&o) noexcept {
    if (this != &o) {
      Destroy(size_);
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
    }
    return *this;
  }
  DpdkArray(const DpdkArray &) = delete;
  DpdkArray &operator=(const DpdkArray &) = delete;

  T &operator[](size_t i) noexcept { return data_[i]; }
  const T &operator[](size_t i) const noexcept { return data_[i]; }
  T *data() noexcept { return data_; }
  const T *data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  size_t bytes() const noexcept { return size_ * sizeof(T); }

 private:
  void Destroy(size_t n) noexcept {
    if (data_ == nullptr) {
      return;
    }
    for (size_t i = 0; i < n; i++) {
      data_[i].~T();
    }
    DpdkFree(data_);
    data_ = nullptr;
  }

  T *data_ = nullptr;
  size_t size_ = 0;
};

// A standard allocator over DPDK memory, for containers that grow on the
// control path (std::vector<T, DpdkAllocator<T>>).
template <typename T>
struct DpdkAllocator {
  using value_type = T;
  int socket = MemoryNode();

  DpdkAllocator() = default;
  explicit DpdkAllocator(int s) : socket(s) {}
  template <typename U>
  DpdkAllocator(const DpdkAllocator<U> &o) noexcept : socket(o.socket) {}

  T *allocate(size_t n) {
    // T is the element type; a container of pointers (MeterSet's index)
    // allocates pointer-sized elements by design.
    // NOLINTNEXTLINE(bugprone-sizeof-expression)
    return static_cast<T *>(DpdkAllocate(n * sizeof(T), alignof(T), socket));
  }
  void deallocate(T *p, size_t) noexcept { DpdkFree(static_cast<void *>(p)); }

  template <typename U>
  bool operator==(const DpdkAllocator<U> &o) const noexcept {
    return socket == o.socket;
  }
};

}  // namespace bess::utils

#endif  // BESS_UTILS_DPDK_MEMORY_H_
