// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

// Benchmark for UrlFilter module.

#include "utils/logging.h"
#include <benchmark/benchmark.h>

#include "url_filter.h"

// Benchmarks the NAT flow hash.
static void BM_FlowHash(benchmark::State& state) {
  Flow f;
  FlowHash h;
  while (state.KeepRunning()) {
    benchmark::DoNotOptimize(h(f));
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_FlowHash);

BENCHMARK_MAIN();
