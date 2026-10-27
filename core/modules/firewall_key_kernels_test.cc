// SPDX-License-Identifier: BSD-3-Clause

#include "modules/firewall_key_kernels.h"
#include "testing/rng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace bess::firewall_key {
namespace {

TEST(FirewallKeyKernelsTest, GathersExactWireFieldsAndPreservesOutputGuards) {
  std::mt19937 rng = bess::testing::SeededRng(0x6512);
  for (size_t offset = 0; offset < 16; ++offset) {
    for (unsigned sample = 0; sample < 1024; ++sample) {
      // Last readable byte is the second destination-port byte. ASan guards
      // the end even when the Ethernet frame begins at an unaligned offset.
      std::vector<uint8_t> input(offset + 38);
      for (auto &byte : input) {
        byte = sample == 0   ? uint8_t{0}
               : sample == 1 ? uint8_t{255}
                             : static_cast<uint8_t>(rng());
      }
      const uint8_t *frame = input.data() + offset;
      std::array<uint8_t, 48> want{};
      want.fill(0x5a);
      std::fill_n(want.data() + 3, 16, uint8_t{0});
      std::fill_n(want.data() + 21, 16, uint8_t{0});
      std::copy_n(frame + 26, 4, want.data() + 3);
      std::copy_n(frame + 30, 4, want.data() + 21);
      const unsigned want_src = unsigned{frame[34]} * 256u + frame[35];
      const unsigned want_dst = unsigned{frame[36]} * 256u + frame[37];
      std::array<uint8_t, 48> actual;
      actual.fill(0x5a);
      uint16_t src_port = 0, dst_port = 0;
      Gather(frame, actual.data() + 3, actual.data() + 21, src_port, dst_port);
      ASSERT_EQ(actual, want);
      ASSERT_EQ(src_port, want_src);
      ASSERT_EQ(dst_port, want_dst);
      actual.fill(0x5a);
      GatherScalar(frame, actual.data() + 3, actual.data() + 21, src_port,
                   dst_port);
      ASSERT_EQ(actual, want);
      ASSERT_EQ(src_port, want_src);
      ASSERT_EQ(dst_port, want_dst);
      actual.fill(0x5a);
      GatherPortable(frame, actual.data() + 3, actual.data() + 21, src_port,
                     dst_port);
      ASSERT_EQ(actual, want);
      ASSERT_EQ(src_port, want_src);
      ASSERT_EQ(dst_port, want_dst);
    }
  }
}

}  // namespace
}  // namespace bess::firewall_key
