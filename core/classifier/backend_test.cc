// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <array>

#include "classifier/backend.h"

namespace {

using bess::classifier::BackendInfo;
using bess::classifier::ConstBytes;
using bess::classifier::ResultSlot;
using bess::classifier::RuntimeExactBackend;
using bess::classifier::RuntimeExactOps;

struct State {
  int calls = 0;
};

uint64_t Lookup(const void *raw, ConstBytes, size_t,
                std::span<ResultSlot> slots) noexcept {
  auto *state = const_cast<State *>(static_cast<const State *>(raw));
  state->calls++;
  uint64_t hits = 0;
  for (size_t i = 0; i < slots.size(); i++) {
    slots[i] = ResultSlot(static_cast<uint32_t>(i + 1));
    hits |= (uint64_t{1} << i);
  }
  return hits;
}

void Destroy(void *raw) noexcept { delete static_cast<State *>(raw); }

TEST(RuntimeBackendTest, DispatchesOncePerBatchAndOwnsState) {
  RuntimeExactBackend<ResultSlot> backend = [] {
    const RuntimeExactOps<ResultSlot> ops{
        Lookup, Destroy, BackendInfo{.rule_count = 3, .key_size = 4}};
    return RuntimeExactBackend<ResultSlot>(ops, new State{});
  }();
  std::array<std::byte, 8> keys{};
  std::array<ResultSlot, 2> slots{};

  const uint64_t hits = backend.lookup_batch(keys, 4, slots);
  EXPECT_EQ(0x3ull, hits);
  EXPECT_EQ(3u, backend.info().rule_count);
  EXPECT_EQ(ResultSlot(1), slots[0]);
  EXPECT_EQ(ResultSlot(2), slots[1]);
}

TEST(RuntimeBackendTest, SupportsGateResultType) {
  using gate_idx_t = uint16_t;
  auto lookup_gate = [](const void *, ConstBytes, size_t,
                        std::span<gate_idx_t> gates) noexcept -> uint64_t {
    if (!gates.empty()) {
      gates[0] = 7;
      return 0x1ull;  // only first hit
    }
    return 0;
  };
  RuntimeExactOps<gate_idx_t> ops{
      .lookup_batch = lookup_gate,
      .destroy = nullptr,
      .info = BackendInfo{.rule_count = 1, .key_size = 4, .result_size = 2},
  };
  RuntimeExactBackend<gate_idx_t> backend(ops, nullptr);
  ASSERT_TRUE(backend);

  std::array<std::byte, 8> keys{};
  std::array<gate_idx_t, 2> gates{};
  uint64_t hits = backend.lookup_batch(keys, 4, gates);
  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(7, gates[0]);
}
}  // namespace
