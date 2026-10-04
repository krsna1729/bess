// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_SIGNAL_CONTEXT_H_
#define BESS_ARCH_SIGNAL_CONTEXT_H_

// Reading the interrupted thread's registers out of a signal handler's
// ucontext_t (M21, D-071). The register layout of mcontext_t is per
// architecture (glibc <sys/ucontext.h>).

#include <ucontext.h>

#include <cstdint>

#include "arch/cpu.h"

namespace bess::arch {

// The program counter the signal interrupted: the faulting instruction for a
// synchronous fault, the next instruction otherwise. 0 when this build does
// not know the architecture's mcontext_t layout (including BESS_ARCH_GENERIC
// builds); callers treat 0 as "unknown" and must not dereference it.
inline uintptr_t ProgramCounter(const ucontext_t &uc) noexcept {
#if defined(BESS_ARCH_X86) && defined(__x86_64__)
  return static_cast<uintptr_t>(uc.uc_mcontext.gregs[REG_RIP]);
#elif defined(BESS_ARCH_X86)
  return static_cast<uintptr_t>(uc.uc_mcontext.gregs[REG_EIP]);
#elif defined(BESS_ARCH_ARM64)
  return static_cast<uintptr_t>(uc.uc_mcontext.pc);
#else
  (void)uc;
  return 0;
#endif
}

}  // namespace bess::arch

#endif  // BESS_ARCH_SIGNAL_CONTEXT_H_
