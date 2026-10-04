// SPDX-License-Identifier: BSD-3-Clause

// Linked into the libFuzzer builds of the fuzz harnesses only.
//
// libclang_rt.fuzzer (Clang 22 as packaged here) carries weak definitions of
// the sized and aligned operator delete forms that just call free(). They are
// linked ahead of ASan's, while every operator new still comes from ASan, so
// ASan reports alloc-dealloc-mismatch on the first aligned delete. The
// mismatch is between two runtimes, not in BESS code; turn that one check off.
// Every other ASan check stays on, and ASAN_OPTIONS still overrides this.

#include "utils/sanitizers.h"

#if BESS_ADDRESS_SANITIZER
extern "C" const char *__asan_default_options() {
  return "alloc_dealloc_mismatch=0";
}
#endif
