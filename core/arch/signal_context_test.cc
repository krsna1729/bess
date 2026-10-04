// SPDX-License-Identifier: BSD-3-Clause

// ProgramCounter() reads the interrupted instruction out of a signal's
// ucontext_t: a trap raised inside a known function must report an address
// inside that function.

#include "arch/signal_context.h"

#include <gtest/gtest.h>

#include <csetjmp>
#include <csignal>
#include <cstdint>

namespace bess::arch {
namespace {

sigjmp_buf trap_return;
volatile uintptr_t trap_pc = 0;

void OnTrap(int, siginfo_t *, void *ucontext) {
  trap_pc = ProgramCounter(*static_cast<const ucontext_t *>(ucontext));
  siglongjmp(trap_return, 1);
}

// x86 UD2 (SIGILL), arm64 BRK (SIGTRAP): a fault whose program counter is
// the trapping instruction, a few bytes into this function.
[[gnu::noinline]] void Trap() {
  __builtin_trap();
}

TEST(SignalContextTest, ProgramCounterIsTheTrappingInstruction) {
  struct sigaction action = {};
  action.sa_sigaction = OnTrap;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  struct sigaction old_ill, old_trap;
  ASSERT_EQ(0, sigaction(SIGILL, &action, &old_ill));
  ASSERT_EQ(0, sigaction(SIGTRAP, &action, &old_trap));

  if (sigsetjmp(trap_return, 1) == 0) {
    Trap();
  }
  sigaction(SIGILL, &old_ill, nullptr);
  sigaction(SIGTRAP, &old_trap, nullptr);

  const uintptr_t start = reinterpret_cast<uintptr_t>(&Trap);
#if defined(BESS_ARCH_X86) || defined(BESS_ARCH_ARM64)
  EXPECT_GE(trap_pc, start);
  EXPECT_LT(trap_pc, start + 64) << std::hex << trap_pc << " vs " << start;
#else
  EXPECT_EQ(0u, trap_pc) << "an unknown layout must report 0, not a guess";
  (void)start;
#endif
}

}  // namespace
}  // namespace bess::arch
