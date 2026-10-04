// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_BITS_H_
#define BESS_UTILS_BITS_H_

#include "utils/logging.h"

#include <algorithm>
#include <cstring>

#include "common.h"

namespace bess {
namespace utils {

// TODO(melvinw): add support for shifting at bit granularity
// Shifts `buf` to the left by `len` bytes and fills in with zeroes using
// std::memmove() and std::memset().
static inline void ShiftBytesLeftSmall(uint8_t *buf, const size_t len,
                                       size_t shift) {
  shift = std::min(shift, len);
  memmove(buf, buf + shift, len - shift);
  memset(buf + len - shift, 0, shift);
}

// TODO(melvinw): add support for shifting at bit granularity
// Shifts `buf` to the left by `len` bytes and fills in with zeroes.
// Will shift in 8-byte chunks if `shift` <= 8, othersize, uses
// std::memmove() and std::memset().
static inline void ShiftBytesLeft(uint8_t *buf, const size_t len,
                                  const size_t shift) {
  if (len < sizeof(uint64_t) || shift > sizeof(uint64_t)) {
    return ShiftBytesLeftSmall(buf, len, shift);
  }

  uint8_t *tmp_buf = buf;
  size_t tmp_len = len;
  size_t inc = sizeof(uint64_t) - shift;
  while (tmp_len >= sizeof(uint64_t)) {
    // Little-endian: shifting the word right moves its bytes toward lower
    // addresses. memcpy: `buf` has no alignment.
    uint64_t block;
    memcpy(&block, tmp_buf, sizeof(block));
    block >>= shift * 8;
    memcpy(tmp_buf, &block, sizeof(block));
    tmp_buf += inc;
    tmp_len = buf + len - tmp_buf;
  }

  buf += len;
  if (static_cast<size_t>(buf - tmp_buf) > shift) {
    tmp_len = buf - tmp_buf - shift;
    memmove(tmp_buf, tmp_buf + shift, tmp_len);
    memset(buf - shift, 0, shift);
  }
}

// TODO(melvinw): add support for shifting at bit granularity
// Shifts `buf` to the right by `len` bytes and fills in with zeroes using
// std::memmove() and std::memset().
static inline void ShiftBytesRightSmall(uint8_t *buf, const size_t len,
                                        size_t shift) {
  shift = std::min(shift, len);
  memmove(buf + shift, buf, len - shift);
  memset(buf, 0, shift);
}

// TODO(melvinw): add support for shifting at bit granularity
// Shifts `buf` to the right by `len` bytes and fills in with zeroes.
// Will shift in 8-byte chunks if `shift` <= 8, othersize, uses
// std::memmove() and std::memset().
static inline void ShiftBytesRight(uint8_t *buf, const size_t len,
                                   const size_t shift) {
  if (len < sizeof(uint64_t) || shift > sizeof(uint64_t)) {
    return ShiftBytesRightSmall(buf, len, shift);
  }

  uint8_t *tmp_buf = buf + len - sizeof(uint64_t);
  size_t dec = sizeof(uint64_t) - shift;
  size_t leftover = len;
  while (tmp_buf >= buf) {
    uint64_t block;
    memcpy(&block, tmp_buf, sizeof(block));
    block <<= shift * 8;
    memcpy(tmp_buf, &block, sizeof(block));
    tmp_buf -= dec;
    leftover -= dec;
  }
  ShiftBytesRightSmall(buf, leftover, shift);
}

// Applies the `len`-byte bitmask `mask` to `buf`, in 1-byte chunks.
static inline void MaskBytesSmall(uint8_t *buf, const uint8_t *mask,
                                  const size_t len) {
  for (size_t i = 0; i < len; i++) {
    buf[i] &= mask[i];
  }
}

// Applies the `len`-byte bitmask `mask` to `buf`, in 8-byte chunks if able,
// otherwise, falls back to 1-byte chunks. Neither pointer needs alignment.
static inline void MaskBytes64(uint8_t *buf, uint8_t const *mask,
                               const size_t len) {
  size_t n = len / sizeof(uint64_t);
  size_t leftover = len - n * sizeof(uint64_t);
  for (size_t i = 0; i < n; i++) {
    uint64_t b, m;
    memcpy(&b, buf, sizeof(b));
    memcpy(&m, mask, sizeof(m));
    b &= m;
    memcpy(buf, &b, sizeof(b));
    buf += sizeof(b);
    mask += sizeof(m);
  }

  if (leftover) {
    MaskBytesSmall(buf, mask, leftover);
  }
}

// Applies the `len`-byte bitmask `mask` to `buf`, in 16-byte chunks if able,
// otherwise, falls back to 8-byte chunks and possibly 1-byte chunks. A 16-byte
// chunk is two 64-bit ANDs, which compilers merge into one vector AND (SSE2
// pand, NEON and).
static inline void MaskBytes(uint8_t *buf, uint8_t const *mask,
                             const size_t len) {
  if (len <= sizeof(uint64_t)) {
    return MaskBytes64(buf, mask, len);
  }

  constexpr size_t kChunk = 2 * sizeof(uint64_t);
  size_t n = len / kChunk;
  size_t leftover = len - n * kChunk;
  for (size_t i = 0; i < n; i++) {
    uint64_t b[2], m[2];
    memcpy(b, buf, kChunk);
    memcpy(m, mask, kChunk);
    b[0] &= m[0];
    b[1] &= m[1];
    memcpy(buf, b, kChunk);
    buf += kChunk;
    mask += kChunk;
  }

  if (leftover >= sizeof(uint64_t)) {
    MaskBytes64(buf, mask, leftover);
  } else {
    MaskBytesSmall(buf, mask, leftover);
  }
}

// Dangerous Helper! Use SetBitsHigh<T>() and SetBitsLow<T>() instead.
template <typename T, typename = std::enable_if<std::is_integral<T>::value>>
static inline T _SetBitsHigh(size_t n) {
  return (T{1} << n) - 1;
}

// TODO(melvinw): it would be nice if SetBitsHigh() supported blobs.
// Returns a mask for the first `n` bits of an integral type `T`, i.e. starting
// from the most significant bit toward the least significant. Returns a `T`
// with all bits set if `n` is greater than the number of bits in `T`.
// For example: SetBitsHigh<uint32_t>(9) will return 0xFF800000
template <typename T, typename = std::enable_if<std::is_integral<T>::value>>
static inline T SetBitsHigh(size_t n) {
  if (unlikely(n == 0)) {
    return T{0};
  }
  return (n >= sizeof(T) * 8) ? ~T{0} : _SetBitsHigh<T>(n);
}

// TODO(melvinw): it would be nice if SetBitsLow() supported blobs.
// Returns a mask for the last `n` bits of an integral type `T`, i.e. starting
// from the least significant bit toward the most signifant. Returns a `T` with
// all bits set if `n` is greater than the number of bits in `T`.
// For example: SetBitsLow<uint32_t>(9) will return 0x000001FF
template <typename T, typename = std::enable_if<std::is_integral<T>::value>>
static inline T SetBitsLow(size_t n) {
  if (unlikely(n == 0)) {
    return T{0};
  }
  return (n >= sizeof(T) * 8) ? ~T{0} : ~_SetBitsHigh<T>((sizeof(T) * 8) - n);
}

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_BITS_H_
