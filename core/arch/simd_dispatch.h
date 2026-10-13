// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_SIMD_DISPATCH_H_
#define BESS_ARCH_SIMD_DISPATCH_H_

// One SIMD width for the whole process, and kernels that pick their
// implementation by it once rather than per call (D-126).
//
// bessd decides the width before the EAL starts (runtime/simd_width.h: the
// --simd_width flag and this CPU) and passes it to SetSimdWidth(); DPDK gets
// the same width as --force-max-simd-bitwidth, so BESS's kernels and DPDK's
// PMD, ACL, FIB and hash paths follow one limit. A kernel with variants for
// several widths is a RuntimeDispatch: its function pointer is chosen when the
// dispatcher is constructed and again whenever the width is set, so a call is
// one indirect call and nothing is tested per packet. A program that never
// sets the width (tests, benchmarks, bess-dev-static programs) runs at
// DefaultSimdWidth().
//
// A width names an ISA level, DPDK's way (enum rte_vect_max_simd): on x86,
// 128 is SSE4.2 (BESS's floor), 256 AVX2, 512 the AVX-512 set of x86-64-v4;
// on arm64, 128 is Advanced SIMD. The width in effect never exceeds the CPU's
// (runtime/simd_width.cc clamps a request), so a variant whose width is at
// most the width in effect only uses instructions the CPU has.
//
// CPU detection describes the CPU, not this build: it ignores
// BESS_ARCH_GENERIC, because DPDK runs the real CPU's paths either way. In a
// BESS_ARCH_GENERIC build the kernels simply declare no vector variants.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>

#include "arch/cpu.h"

namespace bess::arch {

// Vector widths in bits (DPDK's enum rte_vect_max_simd values).
inline constexpr uint16_t kSimdScalar = 64;  // no vector variant
inline constexpr uint16_t kSimd128 = 128;
inline constexpr uint16_t kSimd256 = 256;
inline constexpr uint16_t kSimd512 = 512;

// Whether DetectCpuVectorFeatures() sees every vector width the CPU has, so
// a width beyond it is known to be absent. True on x86 (cpuid); not on
// arm64, where SVE (up to 2048 bits, which DPDK's arm64 paths use) is not
// detected and 128 is only Advanced SIMD's width (D-126).
#if defined(__x86_64__) || defined(__i386__)
inline constexpr bool kCpuSimdWidthKnown = true;
#else
inline constexpr bool kCpuSimdWidthKnown = false;
#endif

// The vector ISA levels a CPU has.
struct CpuVectorFeatures {
  bool vec128 = false;  // x86 SSE4.2; arm64 Advanced SIMD
  bool avx2 = false;
  bool avx512 = false;  // AVX-512 F, BW, CD, DQ and VL (x86-64-v4's set)
};

// The x86 CPUID feature bits the levels are made of.
struct X86CpuFlags {
  bool sse42 = false;
  bool avx2 = false;
  bool avx512f = false;
  bool avx512bw = false;
  bool avx512cd = false;
  bool avx512dq = false;
  bool avx512vl = false;
};

// Pure: 512 needs every one of the five AVX-512 subsets (DPDK's AVX-512
// paths need F and BW at least, ACL also CD and VL).
constexpr CpuVectorFeatures VectorFeaturesFromX86(X86CpuFlags f) noexcept {
  return {.vec128 = f.sse42,
          .avx2 = f.avx2,
          .avx512 = f.avx2 && f.avx512f && f.avx512bw && f.avx512cd && f.avx512dq && f.avx512vl};
}

// This CPU's features (cpuid, as the loader's CPU model reports it, OS
// support for the register state included; arm64 has Advanced SIMD by
// definition).
inline CpuVectorFeatures DetectCpuVectorFeatures() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_cpu_init();  // callable before constructors have run
  return VectorFeaturesFromX86({.sse42 = __builtin_cpu_supports("sse4.2") != 0,
                                .avx2 = __builtin_cpu_supports("avx2") != 0,
                                .avx512f = __builtin_cpu_supports("avx512f") != 0,
                                .avx512bw = __builtin_cpu_supports("avx512bw") != 0,
                                .avx512cd = __builtin_cpu_supports("avx512cd") != 0,
                                .avx512dq = __builtin_cpu_supports("avx512dq") != 0,
                                .avx512vl = __builtin_cpu_supports("avx512vl") != 0});
#elif defined(__aarch64__)
  return {.vec128 = true};
#else
  return {};
#endif
}

// The widest vectors `cpu` runs.
constexpr uint16_t MaxSimdWidth(CpuVectorFeatures cpu) noexcept {
  if (cpu.avx512) {
    return kSimd512;
  }
  if (cpu.avx2) {
    return kSimd256;
  }
  return cpu.vec128 ? kSimd128 : kSimdScalar;
}

// The width a program that never calls SetSimdWidth() runs at: the CPU's,
// at most 256. It is what bessd's --simd_width=auto resolves to on both
// supported architectures (DPDK's own x86 limit is 256; arm64 CPUs top out
// at 128 here).
constexpr uint16_t DefaultSimdWidth(CpuVectorFeatures cpu) noexcept {
  const uint16_t max = MaxSimdWidth(cpu);
  return max < kSimd256 ? max : kSimd256;
}

// One implementation of a kernel and the width it needs.
template <typename Fn>
struct SimdVariant {
  uint16_t width;
  Fn *fn;
};

// The first of `variants` (widest first) whose width is at most `width`; the
// last when none is, so the last variant is the one every CPU runs.
template <typename Fn>
constexpr Fn *SelectSimdVariant(std::span<const SimdVariant<Fn>> variants,
                                uint16_t width) noexcept {
  for (const SimdVariant<Fn> &v : variants) {
    if (v.width <= width) {
      return v.fn;
    }
  }
  return variants.back().fn;
}

namespace detail {

inline void ResolveAll(uint16_t width) noexcept;

// Every live RuntimeDispatch, so SetSimdWidth() can re-resolve them. Inline
// functions with static locals: one registry in bessd and in the plugins it
// loads (bessd exports its symbols; plugins bind to its definitions).
class DispatchNode {
 public:
  DispatchNode(const DispatchNode &) = delete;
  DispatchNode &operator=(const DispatchNode &) = delete;

 protected:
  DispatchNode() = default;
  ~DispatchNode() = default;

  // Links this node (constructors) or unlinks it (destructors; a plugin's
  // dispatchers go when it is unloaded) and returns the width in effect.
  uint16_t Register() noexcept;
  void Unregister() noexcept;

 private:
  friend void ResolveAll(uint16_t width) noexcept;
  virtual void Resolve(uint16_t width) noexcept = 0;

  DispatchNode *next_ = nullptr;
};

struct DispatchState {
  std::mutex mu;
  DispatchNode *head = nullptr;
  uint16_t width = DefaultSimdWidth(DetectCpuVectorFeatures());
};

inline DispatchState &State() noexcept {
  static DispatchState state;
  return state;
}

inline uint16_t DispatchNode::Register() noexcept {
  DispatchState &s = State();
  const std::lock_guard lock(s.mu);
  next_ = s.head;
  s.head = this;
  return s.width;
}

inline void DispatchNode::Unregister() noexcept {
  DispatchState &s = State();
  const std::lock_guard lock(s.mu);
  for (DispatchNode **p = &s.head; *p != nullptr; p = &(*p)->next_) {
    if (*p == this) {
      *p = next_;
      return;
    }
  }
}

inline void ResolveAll(uint16_t width) noexcept {
  DispatchState &s = State();
  const std::lock_guard lock(s.mu);
  s.width = width;
  for (DispatchNode *n = s.head; n != nullptr; n = n->next_) {
    n->Resolve(width);
  }
}

}  // namespace detail

// The width kernels are resolved for.
inline uint16_t SimdWidth() noexcept {
  detail::DispatchState &s = detail::State();
  const std::lock_guard lock(s.mu);
  return s.width;
}

// Sets the width every RuntimeDispatch, present and future, resolves for.
// bessd calls it once, at startup, before any worker runs. Tests may call it
// again (and restore the old width); a call running meanwhile takes either
// implementation.
inline void SetSimdWidth(uint16_t width) noexcept {
  detail::ResolveAll(width);
}

// A kernel with several implementations, chosen by the width in effect:
//
//   inline RuntimeDispatch<unsigned(const uint32_t *, uint32_t) noexcept, 2>
//       match{{{{kSimd256, &MatchAvx2}, {kSimdScalar, &MatchScalar}}}};
//   ... match(hashes, hash) ...
//
// `variants` widest first; the last runs on every CPU. Declare dispatchers
// at namespace scope (an inline variable in a header): calling one before
// its static initialization has run is not supported.
template <typename Fn, size_t N>
class RuntimeDispatch final : detail::DispatchNode {
  static_assert(N > 0, "a kernel needs at least its portable variant");

 public:
  explicit RuntimeDispatch(const std::array<SimdVariant<Fn>, N> &variants) noexcept
      : variants_(variants) {
    fn_.store(SelectSimdVariant<Fn>(variants_, Register()), std::memory_order_relaxed);
  }
  ~RuntimeDispatch() { Unregister(); }
  RuntimeDispatch(const RuntimeDispatch &) = delete;
  RuntimeDispatch &operator=(const RuntimeDispatch &) = delete;

  template <typename... Args>
  decltype(auto) operator()(Args &&...args) const {
    return fn_.load(std::memory_order_relaxed)(std::forward<Args>(args)...);
  }

  // The implementation calls go to, and every one there is (tests).
  Fn *selected() const noexcept { return fn_.load(std::memory_order_relaxed); }
  std::span<const SimdVariant<Fn>> variants() const noexcept { return variants_; }

 private:
  void Resolve(uint16_t width) noexcept override {
    fn_.store(SelectSimdVariant<Fn>(variants_, width), std::memory_order_relaxed);
  }

  std::array<SimdVariant<Fn>, N> variants_;
  // Relaxed: a plain load on x86 and arm64; atomic only so that a width set
  // while another thread calls (a test re-resolving) is not a data race.
  // Either pointer is a correct implementation.
  std::atomic<Fn *> fn_{nullptr};
};

}  // namespace bess::arch

#endif  // BESS_ARCH_SIMD_DISPATCH_H_
