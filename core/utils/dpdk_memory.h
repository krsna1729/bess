// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_DPDK_MEMORY_H_
#define BESS_UTILS_DPDK_MEMORY_H_

#include <malloc.h>

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
// with a declared fallback and a limit"; D-206, "the framework places
// dataplane memory"): every N-scaled table a worker reads comes from DPDK's
// heap (D-029) on the memory node (D-123). Hugepages are used while there
// are any (the cap, the host's pool, the cgroup's hugetlb limit); past them
// a creation takes normal (4 KiB) pages from the "bess-normal" heap instead
// -- slower lookups, reported per resource -- unless bessd was started with
// --mem_fallback=false, which refuses it. Normal-page memory is admitted
// against the process's memory limit (cgroup v2 memory.max, --mem_limit_mb)
// first, so a creation or transaction that would cross it is refused with a
// clear, non-retryable error instead of the kernel OOM-killing bessd.
// Everything here runs on the control path; nothing on the packet path
// changes.

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
  bool fallback = true;      // --mem_fallback (D-206: on unless forbidden)
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
// library users): no limit, normal pages past the hugepages.
void SetMemoryPolicy(MemoryPolicy policy);
const MemoryPolicy &memory_policy() noexcept;

// Normal-page memory about to be committed. Usage is the cgroup's working set
// (memory.current less memory.stat inactive_file: reclaimable page cache is
// not what the OOM killer counts against bessd) when cgroup_dir is set, else
// process RSS plus hugetlb when charged. Throws MemoryRefused(kNormalLimit) if usage + bytes would
// exceed limit - reserve, or what this thread's Headroom allows. No-op
// without a limit. One measurement serves the admissions after it, each
// counted on top, until they add up to 1 MiB (per-node class-C admission,
// D-206); a refusal is always measured afresh.
void AdmitNormalMemory(size_t bytes);

// How much of the reserve this thread's admissions may draw while one lives
// (D-207), for AdmitNormalMemory and the hugepage heap's growth validator.
// The one exception to "limit less the reserve"; a scope only raises the
// level, and restores the one it found.
//  kErase: limit less half the reserve. A transaction that only erases
//    allocates a little before it frees -- its record, the prepay of its
//    removal stage, retirement room -- and must succeed at the limit, where
//    everything else is refused, so a full daemon can always be emptied.
//  kPrepaid: not checked. A removal stage (Retirer::RemoveLater) spends what
//    its erase admitted in Reserve; checking each allocation again would
//    refuse prepaid memory, and a refused stage would wait for memory use to
//    drop, retrying every grace period.
class Headroom {
 public:
  enum Level : uint8_t { kNone, kErase, kPrepaid };
  explicit Headroom(Level level) noexcept;
  ~Headroom();
  Headroom(const Headroom &) = delete;
  Headroom &operator=(const Headroom &) = delete;

 private:
  Level saved_;
};

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
// (the table will come from the fallback heap), or
// MemoryRefused(kHugepagesExhausted) with --mem_fallback=false. Returns
// where the DPDK part goes and arms nothing (MemoryScope arms placement).
// Throws MemoryRefused and counts nothing on refusal. Control path;
// thread-safe.
TableBacking AdmitTable(size_t bytes, size_t glibc_bytes = 0);
// Gives back the DPDK part (bytes - glibc_bytes) AdmitTable admitted, with
// the backing it returned; normal pages are measured, not counted.
void ReleaseTable(TableBacking backing, size_t bytes) noexcept;
// The hugepage bytes AdmitTable has counted and not yet released.
size_t HugepageTableBytes() noexcept;

// Where admitted bytes land (D-206). An admission (framework::MemoryScope)
// arms the calling thread with the DPDK bytes AdmitTable placed and on which
// backing, under a ticket naming the admission's account. The thread's
// next DPDK allocations draw from it: an armed normal-page placement under
// hugepage backing allocates from the fallback heap directly (the cap is
// spent); an armed hugepage placement allocates from the memory node's heap
// and, when that heap cannot give the bytes (hugepages ran out before the
// cap did), from the fallback heap. Bytes that land on the other backing
// than admitted move: the hugepage count follows, and the observer is told
// so the account reports where they are. Unarmed allocations (tests,
// library users, unadmitted DPDK bookkeeping) are placed as before and
// reported nowhere. A later admission on the thread replaces the arming;
// leftover bytes an allocation did not draw are dropped then.
using PlacementObserver = void (*)(uint64_t ticket, size_t bytes,
                                   TableBacking now_on) noexcept;
// The framework's account keeper; set once, before the first admission.
void SetPlacementObserver(PlacementObserver observer) noexcept;
void ArmPlacement(uint64_t ticket, TableBacking backing, size_t bytes) noexcept;
// Whether this thread's admission placed its next bytes on normal pages under
// hugepage backing (the hugepage cap is spent). The EAL must be up.
bool ArmedForNormalPages() noexcept;
// For DPDK libraries that allocate internally (rte_hash, rte_lpm, rte_fib,
// rte_mempool): the socket to create an object of about `bytes` on -- the
// fallback heap's (FallbackSocket) when this thread's admission placed it
// on normal pages under hugepage backing, else `socket`.
int PlacementSocket(int socket, size_t bytes);
// After such an object was created on `socket`: records where its `bytes`
// landed against this thread's arming, as DpdkAllocate does.
void NotePlacement(size_t bytes, int socket) noexcept;
// The same for memory placed outside DPDK's heaps (the packet pool's own
// mapping): `landed` says which backing it is.
void NotePlacement(size_t bytes, TableBacking landed) noexcept;

namespace detail {
// What a hugepage table of `bytes` adds to memory.max when hugetlb is
// charged (D-198): nothing if the heap's largest free block holds it, else
// the whole pages the heap must map for it (DPDK grows the heap by pages).
size_t HugepageCharge(size_t bytes, size_t largest_free, size_t page_size);
// Tests only (D-206): account DPDK's native heap as hugepages (the cap,
// placement, the spill to the fallback heap when the native heap runs out)
// so test binaries, whose EAL runs on normal pages, exercise hugepage
// backing. Off by default; never set in bessd.
void SetNativeHeapIsHugepagesForTest(bool on) noexcept;
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
//
// Shrink takes the DPDK part from the hugepage-placed bytes first (D-206):
// accounts know where an admission landed, not where each freed object was.
class MemoryGrowth {
 public:
  virtual void Grow(size_t bytes, size_t glibc_bytes = 0) = 0;
  virtual void Shrink(size_t bytes, size_t glibc_bytes = 0) noexcept = 0;

 protected:
  ~MemoryGrowth() = default;
};

// What glibc's malloc takes for an allocation of `n` bytes (D-206): the
// chunk, an 8-byte header and the payload rounded up to 16 bytes, at least
// 32 -- a prediction, for a Bytes() that must be known before allocating.
constexpr size_t MallocChunkBytes(size_t n) noexcept {
  const size_t chunk = (n + 8 + 15) & ~size_t{15};
  return chunk < 32 ? 32 : chunk;
}

// What glibc's malloc took for the live block `p` (D-206): its usable size
// as malloc reports it plus the chunk's size word -- measured, not
// predicted. Equals MallocChunkBytes(n) for blocks below the mmap threshold.
inline size_t MallocBytes(void *p) noexcept {
  return malloc_usable_size(p) + sizeof(size_t);
}

// The framework's allocator for cold control-plane state that scales with
// sessions or transactions and that no worker reads (D-206, class C): glibc
// memory, every block charged to `growth` (as glibc bytes) at the size
// malloc actually took (MallocBytes, measured on the block) when it is
// allocated, and given back at that size when it is freed: exact by
// construction. A refused charge frees the block and throws MemoryRefused,
// so the container is unchanged and the scope refuses growth at its limit.
// Null `growth`: unadmitted (tests, library users). The hook must outlive
// every container using the allocator. Packet-path state never uses this:
// it takes DpdkAllocator or an ObjectSlab (DPDK heap, hugepages first).
//
// Future work (D-206, "Class C on the framework allocator"): route these
// containers (UPF F-TEID mirror and session mirrors, MeterResource
// policies_/retiring_, the transaction engine's incoming references)
// through the framework's DPDK allocator too, so placement is uniform.
// Preconditions: (1) fixed-size node
// pools carved from framework chunks (as ObjectSlab), since one rte_malloc
// per node adds about a cache line of header and 64-byte alignment,
// roughly doubling 48-80 B map nodes and fragmenting the heap [unmeasured];
// (2) a hot/cold placement tag the framework decides: cold memory on normal
// pages by default, hugepages only above a reserve kept for hot data, so
// cold growth cannot push later hot-table growth onto normal pages.
// Motivation: about 0.3 GiB at 1M sessions in the graph layout.
template <typename T>
struct AdmittedGlibcAllocator {
  using value_type = T;
  MemoryGrowth *growth = nullptr;

  AdmittedGlibcAllocator() = default;
  explicit AdmittedGlibcAllocator(MemoryGrowth *g) noexcept : growth(g) {}
  template <typename U>
  AdmittedGlibcAllocator(const AdmittedGlibcAllocator<U> &o) noexcept
      : growth(o.growth) {}

  T *allocate(size_t n) {
    T *p = std::allocator<T>().allocate(n);
    if (growth != nullptr) {
      const size_t bytes = MallocBytes(p);
      try {
        growth->Grow(bytes, bytes);
      } catch (...) {
        std::allocator<T>().deallocate(p, n);
        throw;
      }
    }
    return p;
  }
  void deallocate(T *p, size_t n) noexcept {
    const size_t bytes = growth != nullptr ? MallocBytes(p) : 0;
    std::allocator<T>().deallocate(p, n);
    if (growth != nullptr) {
      growth->Shrink(bytes, bytes);
    }
  }
  template <typename U>
  bool operator==(const AdmittedGlibcAllocator<U> &o) const noexcept {
    return growth == o.growth;
  }
};

// For DPDK libraries that allocate internally (rte_hash, rte_lpm, rte_fib,
// rte_ring): call after creating on MemoryNode() failed for lack of memory,
// with an estimate of the bytes the object needs. Returns the socket id of
// the normal-page fallback heap (a DPDK external heap "bess-normal", grown
// and admitted as needed, so at least `bytes` are free in one piece), or
// throws MemoryRefused(kHugepagesExhausted) when fallback is not allowed.
int FallbackSocket(size_t bytes);

// For a DPDK library allocation that failed for lack of memory where no
// fallback applies (the RCU defer queues of rte_hash, rte_lpm and rte_fib,
// and rte_hash's tailq entries, which DPDK takes from its native heaps,
// never the fallback heap): throws
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
  // Admitted hugepage bytes that landed on normal pages because the heap ran
  // out before the cap (D-206), since startup.
  uint64_t spilled_bytes = 0;
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
// normal-page fallback heap unless --mem_fallback=false (always under
// --no-huge, whose fixed heap may fill), else MemoryRefused. Placement
// follows this thread's armed admission (ArmPlacement, D-206).
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

// The framework allocator for packet-path containers (D-206, class P): a
// standard allocator over DpdkAllocate on `socket` (the memory node), so the
// memory policy places it (hugepages first, normal pages past them), with
// every allocation admitted through `growth` first (DPDK bytes, n *
// sizeof(T)) and given back when it is freed. Null `growth`: unadmitted
// (tests, library users); the hook must outlive the container. For
// containers that grow on the control path (std::vector<T,
// DpdkAllocator<T>>); one rte_malloc per allocation, so a container of
// small nodes takes an ObjectSlab instead.
template <typename T>
struct DpdkAllocator {
  using value_type = T;
  int socket = MemoryNode();
  MemoryGrowth *growth = nullptr;

  DpdkAllocator() = default;
  explicit DpdkAllocator(int s, MemoryGrowth *g = nullptr) noexcept
      : socket(s), growth(g) {}
  template <typename U>
  DpdkAllocator(const DpdkAllocator<U> &o) noexcept
      : socket(o.socket), growth(o.growth) {}

  T *allocate(size_t n) {
    // T is the element type; a container of pointers (MeterSet's index)
    // allocates pointer-sized elements by design.
    // NOLINTNEXTLINE(bugprone-sizeof-expression)
    const size_t bytes = n * sizeof(T);
    if (growth != nullptr) {
      growth->Grow(bytes);
    }
    try {
      return static_cast<T *>(DpdkAllocate(bytes, alignof(T), socket));
    } catch (...) {
      if (growth != nullptr) {
        growth->Shrink(bytes);
      }
      throw;
    }
  }
  void deallocate(T *p, size_t n) noexcept {
    DpdkFree(static_cast<void *>(p));
    if (growth != nullptr) {
      // NOLINTNEXTLINE(bugprone-sizeof-expression)
      growth->Shrink(n * sizeof(T));
    }
  }

  template <typename U>
  bool operator==(const DpdkAllocator<U> &o) const noexcept {
    return socket == o.socket && growth == o.growth;
  }
};

}  // namespace bess::utils

#endif  // BESS_UTILS_DPDK_MEMORY_H_
