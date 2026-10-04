// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_CPU_H_
#define BESS_ARCH_CPU_H_

// CPU primitives every architecture provides (M21, D-071): the cache-line
// size, a spin-wait hint and a cycle counter. This directory is the one place
// for architecture branches and intrinsics; libraries call these functions
// (tools/check_arch.py keeps it so).
//
// BESS_ARCH_GENERIC (meson -Darch_generic=true) makes every arch header take
// its portable path, so an x86 build compiles and tests what other
// architectures run.

#include <cstddef>
#include <cstdint>
#include <ctime>

#if !defined(BESS_ARCH_GENERIC) && (defined(__x86_64__) || defined(__i386__))
#define BESS_ARCH_X86 1
#include <x86intrin.h>
#elif !defined(BESS_ARCH_GENERIC) && defined(__aarch64__)
#define BESS_ARCH_ARM64 1
#endif

namespace bess::arch {

// The cache-line size BESS pads and aligns to. DPDK's value when its
// configuration is visible (every BESS translation unit gets
// `-include rte_config.h` from libdpdk's cflags), so BESS and DPDK structures
// agree; otherwise 64 on x86 and 128 on arm64 (DPDK's generic arm64 value:
// some cores have 64-byte lines, padding to 128 only costs space).
#if defined(RTE_CACHE_LINE_SIZE)
inline constexpr size_t kCacheLineSize = RTE_CACHE_LINE_SIZE;
#elif defined(__aarch64__)
inline constexpr size_t kCacheLineSize = 128;
#else
inline constexpr size_t kCacheLineSize = 64;
#endif

// Tells the CPU this thread is spinning (x86 PAUSE, arm64 YIELD). Also a
// compiler barrier, as the asm forms are.
inline void CpuRelax() noexcept {
#if defined(BESS_ARCH_X86)
  _mm_pause();
#elif defined(BESS_ARCH_ARM64)
  asm volatile("yield" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

// A cheap, monotonic counter at a fixed rate (x86 TSC, arm64 CNTVCT_EL0, else
// nanoseconds). Not serializing. Its rate is measured at start-up (`tsc_hz`,
// utils/time.cc), so callers never assume a frequency.
inline uint64_t ReadCycleCounter() noexcept {
#if defined(BESS_ARCH_X86)
  return __rdtsc();
#elif defined(BESS_ARCH_ARM64)
  uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

// ReadCycleCounter() ordered against the surrounding instructions, for timing
// one short operation: every earlier instruction has finished before the read
// and no later one starts until it is done (x86 LFENCE on both sides; arm64
// ISB on both sides; elsewhere only the compiler is held back). Slower than
// ReadCycleCounter(); measure its own cost and subtract it.
inline uint64_t ReadCycleCounterSerialized() noexcept {
#if defined(BESS_ARCH_X86)
  _mm_lfence();
  const uint64_t v = __rdtsc();
  _mm_lfence();
  return v;
#elif defined(BESS_ARCH_ARM64)
  uint64_t v;
  asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(v) : : "memory");
  return v;
#else
  asm volatile("" ::: "memory");
  const uint64_t v = ReadCycleCounter();
  asm volatile("" ::: "memory");
  return v;
#endif
}

// Stops the compiler from moving memory accesses across this point; emits no
// instruction and orders nothing between CPUs.
inline void CompilerBarrier() noexcept {
  asm volatile("" ::: "memory");
}

}  // namespace bess::arch

#endif  // BESS_ARCH_CPU_H_
