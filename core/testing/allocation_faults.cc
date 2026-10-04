// SPDX-License-Identifier: BSD-3-Clause

// The global allocation functions of every test that links this object (see
// allocation_faults.h). Blocks come from malloc/aligned_alloc and go back to
// free, whichever form allocated them.

#include "testing/allocation_faults.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace bess::fault_injection {
namespace internal {
namespace {

constinit thread_local Window *t_window = nullptr;
std::atomic<bool> g_shared_open{false};
std::atomic<size_t> g_injected{0};

Window &Shared() noexcept {
  static Window window;
  return window;
}

// Counts one allocation in `w`; true if it is the one to refuse.
bool Refuse(Window *w) noexcept {
  const size_t index = w->allocations.fetch_add(1, std::memory_order_relaxed);
  if (index != w->fail_at) {
    return false;
  }
  w->injected.store(true, std::memory_order_relaxed);
  g_injected.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Null when a window refuses the allocation (or malloc fails).
void *Allocate(std::size_t n, std::size_t align) noexcept {
  bool refuse = false;
  if (Window *w = t_window; w != nullptr) {
    refuse = Refuse(w);
  }
  if (g_shared_open.load(std::memory_order_acquire)) {
    refuse |= Refuse(&Shared());
  }
  if (refuse) {
    return nullptr;
  }
  if (n == 0) {
    n = 1;
  }
  if (align <= alignof(std::max_align_t)) {
    return std::malloc(n);
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
    w->frees.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_shared_open.load(std::memory_order_acquire)) {
    Shared().frees.fetch_add(1, std::memory_order_relaxed);
  }
  std::free(p);
}

}  // namespace

Window *ExchangeThreadWindow(Window *window) noexcept {
  Window *previous = t_window;
  t_window = window;
  return previous;
}

Window &SharedWindow() noexcept { return Shared(); }

void OpenSharedWindow(size_t fail_at) noexcept {
  if (g_shared_open.load(std::memory_order_relaxed)) {
    std::fputs("AllocationFaults: one all-threads window at a time\n", stderr);
    std::abort();
  }
  Window &w = Shared();
  w.allocations.store(0, std::memory_order_relaxed);
  w.frees.store(0, std::memory_order_relaxed);
  w.injected.store(false, std::memory_order_relaxed);
  w.fail_at = fail_at;
  g_shared_open.store(true, std::memory_order_release);
}

void CloseSharedWindow() noexcept { g_shared_open.store(false, std::memory_order_release); }

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
