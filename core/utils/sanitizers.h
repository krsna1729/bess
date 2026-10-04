// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_SANITIZERS_H_
#define BESS_UTILS_SANITIZERS_H_

// Which sanitizer this translation unit is built with (M22, D-072). GCC
// defines __SANITIZE_ADDRESS__ / __SANITIZE_THREAD__; clang reports them only
// through __has_feature before version 20 (CI's clang-19 among them). Code
// tests BESS_ADDRESS_SANITIZER / BESS_THREAD_SANITIZER (0 or 1), never the
// compiler macros.

#if defined(__SANITIZE_ADDRESS__)
#define BESS_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BESS_ADDRESS_SANITIZER 1
#endif
#endif
#ifndef BESS_ADDRESS_SANITIZER
#define BESS_ADDRESS_SANITIZER 0
#endif

#if defined(__SANITIZE_THREAD__)
#define BESS_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define BESS_THREAD_SANITIZER 1
#endif
#endif
#ifndef BESS_THREAD_SANITIZER
#define BESS_THREAD_SANITIZER 0
#endif

#endif  // BESS_UTILS_SANITIZERS_H_
