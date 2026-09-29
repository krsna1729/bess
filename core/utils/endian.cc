// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "endian.h"

namespace bess {
namespace utils {

bool uint64_to_bin(void *ptr, uint64_t val, size_t size, bool big_endian) {
  uint8_t *const ptr8 = static_cast<uint8_t *>(ptr);

  if (big_endian) {
    for (uint8_t *p = ptr8 + size - 1; p >= ptr8; p--) {
      *p = val & 0xff;
      val >>= 8;
    }
  } else {
    for (uint8_t *p = ptr8; p < ptr8 + size; p++) {
      *p = val & 0xff;
      val >>= 8;
    }
  }

  if (val) {
    return false;  // the value is too large for the size
  } else {
    return true;
  }
}

}  // namespace utils
}  // namespace bess
