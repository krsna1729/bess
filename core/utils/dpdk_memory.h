// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_DPDK_MEMORY_H_
#define BESS_UTILS_DPDK_MEMORY_H_

#include <cstddef>
#include <memory>
#include <new>
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

// Performance-critical dataplane memory comes from DPDK's heap (Decision
// D-029): hugepage-backed when bessd has hugepages, DPDK's normal-page heap
// under --no-huge -- whatever the EAL was initialized with, and one
// allocator rather than a parallel one. On the NUMA socket asked for, by
// default the memory node (SOCKET_ID_ANY: DPDK's choice, the caller's socket
// first); a socket out of memory falls back to any socket, warned once.
// Allocation happens at creation, on the control path; running out is an
// error there (std::bad_alloc), never a packet-path event.
//
// Initializes DPDK first if nothing has yet (tests, benchmarks).
void *DpdkAllocate(size_t bytes, size_t align, int socket = MemoryNode());
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
    return static_cast<T *>(DpdkAllocate(n * sizeof(T), alignof(T), socket));
  }
  void deallocate(T *p, size_t) noexcept { DpdkFree(p); }

  template <typename U>
  bool operator==(const DpdkAllocator<U> &o) const noexcept {
    return socket == o.socket;
  }
};

}  // namespace bess::utils

#endif  // BESS_UTILS_DPDK_MEMORY_H_
