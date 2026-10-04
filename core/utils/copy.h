// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_COPY_H_
#define BESS_UTILS_COPY_H_

#include "utils/logging.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common.h"

namespace bess {
namespace utils {

// Fixed-size copies. A constant-size __builtin_memcpy compiles to unaligned
// vector loads and stores of the widest kind the target has (one 32-byte
// vmovdqu with AVX2, two 16-byte movdqu with SSE, ldp/stp q on arm64), so no
// intrinsics are needed for any architecture.
static inline void Copy16(void *__restrict__ dst,
                          const void *__restrict__ src) {
  __builtin_memcpy(dst, src, 16);
}

static inline void Copy32(void *__restrict__ dst,
                          const void *__restrict__ src) {
  __builtin_memcpy(dst, src, 32);
}

// Copy exactly "bytes" (<= 64). Works best if size is a compile-time constant.
static inline void CopySmall(void *__restrict__ dst,
                             const void *__restrict__ src, size_t bytes) {
  DCHECK_LE(bytes, 64);

  auto *d = reinterpret_cast<char *__restrict__>(dst);
  auto *s = reinterpret_cast<const char *__restrict__>(src);

  if (32 < bytes) {
    Copy32(d, s);
    Copy32(d + bytes - 32, s + bytes - 32);
    return;
  }

  if (16 < bytes) {
    Copy16(d, s);
    Copy16(d + bytes - 16, s + bytes - 16);
    return;
  }

  switch (bytes) {
    case 16:
      Copy16(d, s);
      break;
    case 15:
      memcpy(d, s, 8);
      memcpy(d + 7, s + 7, 8);
      break;
    case 14:
      memcpy(d, s, 8);
      memcpy(d + 6, s + 6, 8);
      break;
    case 13:
      memcpy(d, s, 8);
      memcpy(d + 5, s + 5, 8);
      break;
    case 12:
      memcpy(d, s, 12);
      break;
    case 11:
      memcpy(d, s, 8);
      memcpy(d + 7, s + 7, 4);
      break;
    case 10:
      memcpy(d, s, 10);
      break;
    case 9:
      memcpy(d + 8, s + 8, 1);
      [[fallthrough]];
    case 8:
      memcpy(d, s, 8);
      break;
    case 7:
      memcpy(d, s, 4);
      memcpy(d + 3, s + 3, 4);
      break;
    case 6:
      memcpy(d, s, 6);
      break;
    case 5:
      memcpy(d + 4, s + 4, 1);
      [[fallthrough]];
    case 4:
      memcpy(d, s, 4);
      break;
    case 3:
      memcpy(d + 2, s + 2, 1);
      [[fallthrough]];
    case 2:
      memcpy(d, s, 2);
      break;
    case 1:
      memcpy(d, s, 1);
      break;
  }
}

// Inline version of Copy(). Use only when performance is critial. Since the
// function is inlined whenever used, the compiled code will be substantially
// larger. See Copy() for more details.
static inline void CopyInlined(void *__restrict__ dst,
                               const void *__restrict__ src, size_t bytes,
                               bool sloppy = false) {
  // The bulk loop moves 32-byte blocks on every target, so a sloppy copy
  // overruns by at most 31 bytes everywhere (see Copy()).
  constexpr size_t block_size = 32;
  auto *d = static_cast<char *__restrict__>(dst);
  auto *s = static_cast<const char *__restrict__>(src);

  if (bytes <= 64 && !sloppy) {
    CopySmall(d, s, bytes);
    return;
  }

  // Align dst on a block boundary if buffer is big yet misaligned.
  uintptr_t misalign = reinterpret_cast<uintptr_t>(d) % block_size;
  if (bytes >= 256 && misalign != 0) {
    // Copy a whole block, but proceed with only "offset" bytes.
    Copy32(d, s);

    size_t offset = block_size - misalign;
    d += offset;
    s += offset;
    bytes -= offset;
  }

  size_t num_blocks = (sloppy ? bytes + block_size - 1 : bytes) / block_size;
  size_t num_loops = num_blocks / 8;

  while (num_loops--) {
    Copy32(d + 0 * block_size, s + 0 * block_size);
    Copy32(d + 1 * block_size, s + 1 * block_size);
    Copy32(d + 2 * block_size, s + 2 * block_size);
    Copy32(d + 3 * block_size, s + 3 * block_size);
    Copy32(d + 4 * block_size, s + 4 * block_size);
    Copy32(d + 5 * block_size, s + 5 * block_size);
    Copy32(d + 6 * block_size, s + 6 * block_size);
    Copy32(d + 7 * block_size, s + 7 * block_size);
    d += 8 * block_size;
    s += 8 * block_size;
  }

  // Copy the leftover. No block to copy if remainder is 0
  size_t leftover_blocks = num_blocks % 8;

  switch (leftover_blocks) {
    case 7:
      Copy32(d + 6 * block_size, s + 6 * block_size);
      [[fallthrough]];
    case 6:
      Copy32(d + 5 * block_size, s + 5 * block_size);
      [[fallthrough]];
    case 5:
      Copy32(d + 4 * block_size, s + 4 * block_size);
      [[fallthrough]];
    case 4:
      Copy32(d + 3 * block_size, s + 3 * block_size);
      [[fallthrough]];
    case 3:
      Copy32(d + 2 * block_size, s + 2 * block_size);
      [[fallthrough]];
    case 2:
      Copy32(d + 1 * block_size, s + 1 * block_size);
      [[fallthrough]];
    case 1:
      Copy32(d + 0 * block_size, s + 0 * block_size);
  }

  // The last partial block: copy the block that ends exactly at "bytes",
  // overlapping what was already copied.
  if (!sloppy && (bytes % block_size) != 0) {
    size_t fringe = bytes % block_size;
    size_t end = leftover_blocks * block_size + fringe;
    Copy32(d + end - block_size, s + end - block_size);
  }
}

// Non-inlined version of Copy().
// Do not call this function directly, unless you know what you are doing.
// Just use Copy()
void CopyNonInlined(void *__restrict__ dst, const void *__restrict__ src,
                    size_t bytes, bool sloppy = false);

// Copies "bytes" data from "src" to "dst".
// Same as memcpy() and rte_memcpy(), but significantly faster for both
// aligned/unaligned buffers. Performs best if aligned, of course.
// bytes can be 0.
//
// NOTE: When "sloppy" is set, it may copy more than "bytes", up to additional
// 31 bytes. It will generate much smaller and usually faster code. Use this
// option only if overwriting some data at the end is acceptable, such as
// rewriting packet payload data through PacketRef.
static inline void Copy(void *__restrict__ dst, const void *__restrict__ src,
                        size_t bytes, bool sloppy = false) {
  // If the size is a compile-time constant, inlining can generate compact code
  if (__builtin_constant_p(bytes)) {
    CopyInlined(dst, src, bytes, sloppy);
  } else {
    CopyNonInlined(dst, src, bytes, sloppy);
  }
}

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_COPY_H_
