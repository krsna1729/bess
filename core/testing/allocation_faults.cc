// SPDX-License-Identifier: BSD-3-Clause

// The global allocation functions of every test that links this object (see
// allocation_faults.h). Blocks come from malloc/aligned_alloc and go back to
// free, whichever form allocated them.

#include "testing/allocation_faults.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace bess::fault_injection {
namespace internal {
namespace {

constinit thread_local Window *t_window = nullptr;
std::atomic<size_t> g_injected{0};

// Null when the thread's window refuses the allocation (or malloc fails).
void *Allocate(std::size_t n, std::size_t align) noexcept {
  if (Window *w = t_window; w != nullptr && w->allocations++ == w->fail_at) {
    w->injected = true;
    g_injected.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  if (n == 0) {
    n = 1;
  }
  if (align <= alignof(std::max_align_t)) {
    return std::malloc(n);
  }
  // aligned_alloc wants a multiple of the alignment; rounding up must not wrap.
  if (n > SIZE_MAX - align) {
    return nullptr;
  }
  return std::aligned_alloc(align, (n + align - 1) / align * align);
}

void *AllocateOrThrow(std::size_t n, std::size_t align) {
  if (void *p = Allocate(n, align)) {
    return p;
  }
  throw std::bad_alloc();
}

void Release(void *p) noexcept {
  if (p == nullptr) {
    return;
  }
  if (Window *w = t_window; w != nullptr) {
    w->frees++;
  }
  std::free(p);
}

}  // namespace

Window *ExchangeThreadWindow(Window *window) noexcept {
  Window *previous = t_window;
  t_window = window;
  return previous;
}

}  // namespace internal

size_t InjectedFailures() noexcept {
  return internal::g_injected.load(std::memory_order_relaxed);
}

}  // namespace bess::fault_injection

using bess::fault_injection::internal::Allocate;
using bess::fault_injection::internal::AllocateOrThrow;
using bess::fault_injection::internal::Release;

// Replacing the global allocation functions pairs malloc with free by design;
// GCC's -Wmismatched-new-delete does not know these are the replacements.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

void *operator new(std::size_t n) { return AllocateOrThrow(n, 0); }
void *operator new[](std::size_t n) { return AllocateOrThrow(n, 0); }
void *operator new(std::size_t n, std::align_val_t a) {
  return AllocateOrThrow(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a) {
  return AllocateOrThrow(n, static_cast<std::size_t>(a));
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return Allocate(n, 0); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return Allocate(n, 0); }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept {
  return Allocate(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept {
  return Allocate(n, static_cast<std::size_t>(a));
}

void operator delete(void *p) noexcept { Release(p); }
void operator delete[](void *p) noexcept { Release(p); }
void operator delete(void *p, std::size_t) noexcept { Release(p); }
void operator delete[](void *p, std::size_t) noexcept { Release(p); }
void operator delete(void *p, std::align_val_t) noexcept { Release(p); }
void operator delete[](void *p, std::align_val_t) noexcept { Release(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { Release(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { Release(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { Release(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { Release(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { Release(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept {
  Release(p);
}

#pragma GCC diagnostic pop
