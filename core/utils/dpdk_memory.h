// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_UTILS_DPDK_MEMORY_H_
#define BESS_UTILS_DPDK_MEMORY_H_

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <rte_memory.h>

namespace bess::utils {

// Performance-critical dataplane memory comes from DPDK's heap (Decision
// D-029): hugepage-backed when bessd has hugepages, DPDK's normal-page heap
// under --no-huge -- whatever the EAL was initialized with, and one
// allocator rather than a parallel one. On the NUMA socket asked for
// (SOCKET_ID_ANY: DPDK's choice, the caller's socket first). Allocation
// happens at creation, on the control path; running out is an error there
// (std::bad_alloc), never a packet-path event.
//
// Initializes DPDK first if nothing has yet (tests, benchmarks).
void *DpdkAllocate(size_t bytes, size_t align, int socket = SOCKET_ID_ANY);
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
  DpdkArray(size_t n, int socket = SOCKET_ID_ANY) : size_(n) {
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
  int socket = SOCKET_ID_ANY;

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
