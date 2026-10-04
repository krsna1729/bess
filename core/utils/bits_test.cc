// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "bits.h"

#include <gtest/gtest.h>

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintBytes(const std::string &label, const std::vector<uint8_t> &buf) {
  std::cout << label << ": ";
  for (uint8_t b : buf) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << int(b) << " ";
  }
  std::cout << std::endl;
}

void SetupBuffers(std::vector<uint8_t> *a, std::vector<uint8_t> *b,
                  size_t len) {
  a->clear();
  b->clear();
  for (size_t i = 0; i < len; i++) {
    a->push_back(i + 1);
    b->push_back(i + 1);
  }
}

// Shifting ------------------------------------------------------------------

TEST(ShiftRight, ShortBuffer) {
  const size_t kLength = 5;
  std::vector<std::vector<uint8_t>> exp = {
      {0xAA, 0xBB, 0xCC, 0xDD, 0xEE}, {0x00, 0xAA, 0xBB, 0xCC, 0xDD},
      {0x00, 0x00, 0xAA, 0xBB, 0xCC}, {0x00, 0x00, 0x00, 0xAA, 0xBB},
      {0x00, 0x00, 0x00, 0x00, 0xAA}, {0x00, 0x00, 0x00, 0x00, 0x00}};
  for (size_t i = 0; i < exp.size(); i++) {
    std::vector<uint8_t> buf = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    bess::utils::ShiftBytesRightSmall(buf.data(), kLength, i);
    int ret = memcmp(exp[i].data(), buf.data(), kLength);
    if (ret != 0) {
      EXPECT_TRUE(false) << "equality check failed for shift: " << i;
      PrintBytes("buf", buf);
      PrintBytes("exp", exp[i]);
    }
  }
}

TEST(ShiftRight, Aligned) {
  std::vector<size_t> lengths = {8, 16, 24, 32};
  std::vector<size_t> shifts = {1, 2, 3, 5, 7, 13};
  std::vector<uint8_t> buf, exp;

  for (size_t len : lengths) {
    for (size_t shift : shifts) {
      SetupBuffers(&buf, &exp, len);
      bess::utils::ShiftBytesRightSmall(exp.data(), len, shift);
      bess::utils::ShiftBytesRight(buf.data(), len, shift);
      int ret = memcmp(exp.data(), buf.data(), len);
      if (ret != 0) {
        EXPECT_TRUE(false) << "equality check failed for len: " << len
                           << ", shift: " << shift;
        PrintBytes("buf", buf);
        PrintBytes("exp", exp);
      }
    }
  }
}

TEST(ShiftRight, Unaligned) {
  std::vector<size_t> lengths = {9, 10, 11, 12, 13, 14, 15};
  std::vector<size_t> shifts = {1, 2, 3, 5, 7, 13};
  std::vector<uint8_t> buf, exp;

  for (size_t len : lengths) {
    for (size_t shift : shifts) {
      SetupBuffers(&buf, &exp, len);
      bess::utils::ShiftBytesRightSmall(exp.data(), len, shift);
      bess::utils::ShiftBytesRight(buf.data(), len, shift);
      int ret = memcmp(exp.data(), buf.data(), len);
      if (ret != 0) {
        EXPECT_TRUE(false) << "equality check failed for len: " << len
                           << ", shift: " << shift;
        PrintBytes("buf", buf);
        PrintBytes("exp", exp);
      }
    }
  }
}

TEST(ShiftLeft, ShortBuffer) {
  const size_t kLength = 5;
  std::vector<std::vector<uint8_t>> exp = {
      {0xAA, 0xBB, 0xCC, 0xDD, 0xEE}, {0xBB, 0xCC, 0xDD, 0xEE, 0x00},
      {0xCC, 0xDD, 0xEE, 0x00, 0x00}, {0xDD, 0xEE, 0x00, 0x00, 0x00},
      {0xEE, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00}};
  for (size_t i = 0; i < exp.size(); i++) {
    std::vector<uint8_t> buf = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    bess::utils::ShiftBytesLeftSmall(buf.data(), kLength, i);
    int ret = memcmp(exp[i].data(), buf.data(), kLength);
    if (ret != 0) {
      EXPECT_TRUE(false) << "equality check failed for shift: " << i;
      PrintBytes("buf", buf);
      PrintBytes("exp", exp[i]);
    }
  }
}

TEST(ShiftLeft, Aligned) {
  std::vector<size_t> lengths = {8, 16, 24, 32};
  std::vector<size_t> shifts = {1, 2, 3, 5, 7, 13};
  std::vector<uint8_t> buf, exp;

  for (size_t len : lengths) {
    for (size_t shift : shifts) {
      SetupBuffers(&buf, &exp, len);
      bess::utils::ShiftBytesLeftSmall(exp.data(), len, shift);
      bess::utils::ShiftBytesLeft(buf.data(), len, shift);
      int ret = memcmp(exp.data(), buf.data(), len);
      if (ret != 0) {
        EXPECT_TRUE(false) << "equality check failed for len: " << len
                           << ", shift: " << shift;
        PrintBytes("buf", buf);
        PrintBytes("exp", exp);
      }
    }
  }
}

TEST(ShiftLeft, Unaligned) {
  std::vector<size_t> lengths = {9, 10, 11, 12, 13, 14, 15};
  std::vector<size_t> shifts = {1, 2, 3, 5, 7, 13};
  std::vector<uint8_t> buf, exp;

  for (size_t len : lengths) {
    for (size_t shift : shifts) {
      SetupBuffers(&buf, &exp, len);
      bess::utils::ShiftBytesLeftSmall(exp.data(), len, shift);
      bess::utils::ShiftBytesLeft(buf.data(), len, shift);
      int ret = memcmp(exp.data(), buf.data(), len);
      if (ret != 0) {
        EXPECT_TRUE(false) << "equality check failed for len: " << len
                           << ", shift: " << shift;
        PrintBytes("buf", buf);
        PrintBytes("exp", exp);
      }
    }
  }
}

// Masking -------------------------------------------------------------------
TEST(Mask, SmallAllBits) {
  const size_t kLength = 5;
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05};
  std::vector<uint8_t> mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::vector<uint8_t> exp = {0x01, 0x02, 0x03, 0x04, 0x05};

  bess::utils::MaskBytesSmall(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

TEST(Mask, SmallNoBits) {
  const size_t kLength = 5;
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05};
  std::vector<uint8_t> mask = {0x00, 0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> exp = {0x00, 0x00, 0x00, 0x00, 0x00};

  bess::utils::MaskBytesSmall(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

TEST(Mask, SmallSomeBits) {
  const size_t kLength = 5;
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05};
  std::vector<uint8_t> mask = {0x00, 0x00, 0xFF, 0x00, 0x00};
  std::vector<uint8_t> exp = {0x00, 0x00, 0x03, 0x00, 0x00};

  bess::utils::MaskBytesSmall(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

TEST(Mask, LongAligned) {
  const size_t kLength = 8;
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  std::vector<uint8_t> mask = {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00};
  std::vector<uint8_t> exp = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00};

  bess::utils::MaskBytes(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

TEST(Mask, LongUnaligned) {
  const size_t kLength = 10;
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05,
                              0x06, 0x07, 0x08, 0x09, 0x0A};
  std::vector<uint8_t> mask = {0x00, 0x00, 0x00, 0x00, 0x00,
                               0xFF, 0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> exp = {0x00, 0x00, 0x00, 0x00, 0x00,
                              0x06, 0x00, 0x00, 0x00, 0x00};

  bess::utils::MaskBytes(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

TEST(Mask, ExtraLongAligned) {
  const size_t kLength = 32;
  std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                              0x07, 0x08, 0x09, 0x0A, 0x01, 0x02, 0x03, 0x04,
                              0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x01, 0x02};
  std::vector<uint8_t> mask = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> exp = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00};

  bess::utils::MaskBytes(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

TEST(Mask, ExtraLongUnAligned) {
  const size_t kLength = 33;
  std::vector<uint8_t> buf = {
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x01,
      0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x01, 0x02,
      0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x01, 0x02, 0xAB};
  std::vector<uint8_t> mask = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF};
  std::vector<uint8_t> exp = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0xAB};

  bess::utils::MaskBytes(buf.data(), mask.data(), kLength);
  int ret = memcmp(exp.data(), buf.data(), kLength);
  if (ret != 0) {
    EXPECT_TRUE(false) << "equality check failed";
    PrintBytes("buf", buf);
    PrintBytes("exp", exp);
  }
}

// Every length across the 1-, 8- and 16-byte chunk boundaries, at every
// relative misalignment of buf and mask: each byte is ANDed with its mask
// byte and nothing outside [buf, buf + len) changes.
TEST(Mask, EveryLengthAndAlignment) {
  const size_t kMaxLength = 80;
  const size_t kPad = 16;
  for (size_t len = 0; len <= kMaxLength; len++) {
    for (size_t buf_off = 0; buf_off < 8; buf_off++) {
      for (size_t mask_off = 0; mask_off < 8; mask_off += 3) {
        std::vector<uint8_t> buf(kPad + kMaxLength + kPad);
        std::vector<uint8_t> mask(kMaxLength + 8);
        for (size_t i = 0; i < buf.size(); i++) {
          buf[i] = static_cast<uint8_t>(0x5A ^ (i * 37));
        }
        for (size_t i = 0; i < mask.size(); i++) {
          mask[i] = static_cast<uint8_t>(i * 101 + len);
        }
        std::vector<uint8_t> exp = buf;
        uint8_t *b = buf.data() + kPad + buf_off;
        const uint8_t *m = mask.data() + mask_off;
        for (size_t i = 0; i < len; i++) {
          exp[kPad + buf_off + i] &= m[i];
        }
        std::vector<uint8_t> orig = buf;
        bess::utils::MaskBytes(b, m, len);
        ASSERT_EQ(exp, buf) << "len=" << len << " buf_off=" << buf_off
                            << " mask_off=" << mask_off;
        buf = orig;
        b = buf.data() + kPad + buf_off;
        bess::utils::MaskBytes64(b, m, len);
        ASSERT_EQ(exp, buf) << "MaskBytes64 len=" << len
                            << " buf_off=" << buf_off << " mask_off=" << mask_off;
      }
    }
  }
}

}  // namespace (unnamed)
