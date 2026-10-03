// SPDX-License-Identifier: BSD-3-Clause

// The Router module's per-packet decision (modules/router.cc ProcessBatch): a
// burst of next-hop ids resolved with LookupNextHops, then one gate per packet.
//
//   BM_HopToGate/0  the gate is the next hop's egress as stored (what the
//                   module did before M7, when egress was a gate_idx_t)
//   BM_HopToGate/1  the gate is GateOf(egress) (M7's interface -> gate mapping)
//
// Both read the same next hops; the difference is the mapping's cost (D-049,
// D-060). Arg 2 is the number of next hops the ids range over.

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "gate.h"
#include "modules/router_gate_map.h"
#include "route/router.h"
#include "runtime/runtime_state.h"

namespace {

using bess::gate_idx_t;
using bess::route::NeighborState;
using bess::route::NextHop;
using bess::route::NextHopId;
using bess::route::Router;

constexpr size_t kBurst = 32;
constexpr size_t kIds = 32768;

void BM_HopToGate(benchmark::State &state) {
  const bool mapped = state.range(0) == 1;
  const auto hops_n = static_cast<uint32_t>(state.range(1));
  Router::Config config;
  config.max_routes = 64;
  config.tbl8_groups = 16;
  auto router =
      Router::Create("router_bench_rt", config, hops_n + 1, bess::runtime::runtime().rcu())
          .value();
  for (uint32_t id = 1; id <= hops_n; id++) {
    NextHop hop;
    hop.egress = bess::dataplane::InterfaceId(static_cast<uint16_t>(id % 64 + 1));
    hop.neighbor = NeighborState::kResolved;
    (void)router->SetNextHop(NextHopId(id), hop);
  }
  std::mt19937 rng(0x6a7e);
  std::vector<NextHopId> ids(kIds);
  for (auto &id : ids) {
    id = NextHopId(1 + rng() % hops_n);
  }
  std::array<const NextHop *, kBurst> hops;
  std::array<gate_idx_t, kBurst> gates;
  size_t offset = 0;
  for (auto _ : state) {
    const uint64_t resolved =
        router->LookupNextHops(std::span(&ids[offset], kBurst), std::span(hops));
    for (size_t i = 0; i < kBurst; i++) {
      const NextHop *hop = (resolved & (uint64_t{1} << i)) != 0 ? hops[i] : nullptr;
      if (hop == nullptr || hop->neighbor != NeighborState::kResolved) {
        gates[i] = DROP_GATE;
      } else if (mapped) {
        gates[i] = bess::modules::router::GateOf(hop->egress);
      } else {
        gates[i] = static_cast<gate_idx_t>(hop->egress.value());
      }
    }
    benchmark::DoNotOptimize(gates);
    offset = (offset + kBurst) % kIds;
  }
  state.SetItemsProcessed(state.iterations() * kBurst);
}
BENCHMARK(BM_HopToGate)->ArgsProduct({{0, 1}, {1024, 65536}});

}  // namespace
