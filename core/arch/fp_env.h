// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_FP_ENV_H_
#define BESS_ARCH_FP_ENV_H_

// The floating-point environment's denormal handling (M21, D-071).

#include <cstdint>

#include "arch/cpu.h"

#if defined(BESS_ARCH_X86)
#include <xmmintrin.h>
#endif

namespace bess::arch {

// Whether ScopedFlushDenormals changes anything in this build.
#if defined(BESS_ARCH_X86) || defined(BESS_ARCH_ARM64)
inline constexpr bool kCanFlushDenormals = true;
#else
inline constexpr bool kCanFlushDenormals = false;
#endif

// For its lifetime, makes this thread's floating-point unit treat denormal
// inputs and results as zero (x86 MXCSR DAZ|FTZ, arm64 FPCR.FZ), as
// -ffast-math start-up code and some libraries do. Tests use it to show that
// a kernel's result does not depend on the floating-point environment.
class ScopedFlushDenormals {
 public:
  ScopedFlushDenormals() noexcept {
#if defined(BESS_ARCH_X86)
    saved_ = _mm_getcsr();
    _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);  // DAZ | FTZ
#elif defined(BESS_ARCH_ARM64)
    asm volatile("mrs %0, fpcr" : "=r"(saved_));
    asm volatile("msr fpcr, %0" ::"r"(saved_ | (uint64_t{1} << 24)));  // FZ
#endif
  }
  ~ScopedFlushDenormals() {
#if defined(BESS_ARCH_X86)
    _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(BESS_ARCH_ARM64)
    asm volatile("msr fpcr, %0" ::"r"(saved_));
#endif
  }
  ScopedFlushDenormals(const ScopedFlushDenormals &) = delete;
  ScopedFlushDenormals &operator=(const ScopedFlushDenormals &) = delete;

 private:
  [[maybe_unused]] uint64_t saved_ = 0;
};

}  // namespace bess::arch

#endif  // BESS_ARCH_FP_ENV_H_
