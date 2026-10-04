// SPDX-License-Identifier: BSD-3-Clause

// The architecture primitives (M21, D-071) agree with their portable
// definitions, whichever path this build takes.

#include "arch/cpu.h"
#include "arch/crc32c.h"

#include <gtest/gtest.h>

#include <rte_crc_sw.h>

#include <bit>
#include <random>

namespace bess::arch {
namespace {

TEST(ArchTest, Crc32cMatchesTheSoftwareDefinition) {
  std::mt19937_64 rng(71);
  for (int i = 0; i < 100000; i++) {
    const uint64_t v = i < 4 ? (i == 0 ? 0 : ~uint64_t{0} >> (i * 8)) : rng();
    const uint32_t init = i & 1 ? static_cast<uint32_t>(rng()) : 0xffffffffu;
    ASSERT_EQ(crc32c_2words(v, init), Crc32c(v, init)) << std::hex << v;
    ASSERT_EQ(crc32c_1word(static_cast<uint32_t>(v), init), Crc32c(static_cast<uint32_t>(v), init));
    ASSERT_EQ(crc32c_2bytes(static_cast<uint16_t>(v), init), Crc32c(static_cast<uint16_t>(v), init));
  }
  // The published CRC32C check value: "123456789" with init and final XOR
  // ~0 gives 0xe3069283 (RFC 3720 B.4 uses the same polynomial).
  const char *s = "123456789";
  uint32_t c = 0xffffffffu;
  for (int i = 0; i + 2 <= 9; i += 2) {
    c = Crc32c(static_cast<uint16_t>(uint8_t(s[i]) | uint8_t(s[i + 1]) << 8), c);
  }
  c = crc32c_1byte(uint8_t(s[8]), c);
  EXPECT_EQ(0xe3069283u, ~c);
}

TEST(ArchTest, CycleCounterAdvancesMonotonically) {
  uint64_t prev = ReadCycleCounter();
  bool advanced = false;
  for (int i = 0; i < 1000000 && !advanced; i++) {
    CpuRelax();
    const uint64_t now = ReadCycleCounter();
    ASSERT_GE(now, prev);
    advanced = now > prev;
    prev = now;
  }
  EXPECT_TRUE(advanced);
}

TEST(ArchTest, CacheLineIsDpdksAndAPowerOfTwo) {
  EXPECT_TRUE(std::has_single_bit(kCacheLineSize));
  EXPECT_GE(kCacheLineSize, 64u);
#if defined(RTE_CACHE_LINE_SIZE)
  EXPECT_EQ(size_t{RTE_CACHE_LINE_SIZE}, kCacheLineSize);
#endif
}

}  // namespace
}  // namespace bess::arch
