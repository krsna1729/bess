// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// RCU benchmarks (K1.7).
//
// The read path is the one that must not regress: `RcuPtr<T>::Read()` has to
// cost what a raw atomic pointer load costs, because it is what a packet
// worker does once per batch. The publish path is measured for the opposite
// reason -- it is allowed to be expensive (it builds a table), but it must not
// wait for readers, and it must not grow the retirement queue without bound.
//
// Needs no EAL: the domain is ordinary memory, and no worker participates in
// these benchmarks (a reader is registered and reports quiescence inline).

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <vector>

#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

namespace {

using bess::rcu::GracePeriod;
using bess::rcu::RcuDomain;
using bess::rcu::RcuPtr;

constexpr uint32_t kMaxReaders = 64;

// A generation shaped like the real ones: a pointer plus a little state.
struct Table {
  const void *lpm = nullptr;
  int default_gate = 0;
  int size = 0;
};

constexpr size_t kBatchesPerIteration = 256;

// The baseline: a raw atomic pointer load, once per batch.
void BM_RawAtomicLoad(benchmark::State &state) {
  static Table table;
  static std::atomic<const Table *> published{&table};

  for (auto _ : state) {
    for (size_t i = 0; i < kBatchesPerIteration; i++) {
      const Table *t = published.load(std::memory_order_acquire);
      benchmark::DoNotOptimize(t);
    }
  }
}
BENCHMARK(BM_RawAtomicLoad);

// What a worker actually calls.
void BM_RcuPtrRead(benchmark::State &state) {
  RcuDomain domain(kMaxReaders);
  RcuPtr<Table> published(domain);
  published.Initialize(std::make_unique<const Table>());

  for (auto _ : state) {
    for (size_t i = 0; i < kBatchesPerIteration; i++) {
      const Table *t = published.Read();
      benchmark::DoNotOptimize(t);
    }
  }
}
BENCHMARK(BM_RcuPtrRead);

// Publishing with a reader that reports quiescence inline: the cost of the
// control-side path when nothing is waiting on it.
void BM_PublishWithIdleReader(benchmark::State &state) {
  RcuDomain domain(kMaxReaders);
  RcuPtr<Table> published(domain);
  published.Initialize(std::make_unique<const Table>());

  const uint32_t reader = 1;
  domain.Register(reader);
  domain.Online(reader);

  for (auto _ : state) {
    domain.Quiescent(reader);
    published.Publish(std::make_unique<const Table>());
    domain.ReclaimReady();
  }

  domain.Offline(reader);
  domain.Unregister(reader);
  domain.Drain();
}
BENCHMARK(BM_PublishWithIdleReader);

// Publishing while a reader stays online: reclamation is deferred, so the
// queue grows until the reader reports. This measures the control-side cost
// when nothing can be reclaimed yet, which is the pathological case the queue
// bound exists for.
void BM_PublishWithReaderHolding(benchmark::State &state) {
  RcuDomain domain(kMaxReaders, /*retire_high_water=*/4096);
  RcuPtr<Table> published(domain);
  published.Initialize(std::make_unique<const Table>());

  const uint32_t reader = 1;
  domain.Register(reader);
  domain.Online(reader);

  for (auto _ : state) {
    published.Publish(std::make_unique<const Table>());
    if (domain.Stats().pending_retired_objects > 1024) {
      // The bound is doing its job: the control side pays for reclamation.
      domain.Quiescent(reader);
      domain.ReclaimReady();
    }
  }

  domain.Quiescent(reader);
  domain.Offline(reader);
  domain.Unregister(reader);
  domain.Drain();
}
BENCHMARK(BM_PublishWithReaderHolding);

// Grace-period latency characterization: how long a publication waits for a
// reader that reports quiescence inline (one reader, no workers).
void BM_GracePeriodLatency(benchmark::State &state) {
  RcuDomain domain(kMaxReaders);
  const uint32_t reader = 1;
  domain.Register(reader);
  domain.Online(reader);

  for (auto _ : state) {
    const GracePeriod token = domain.StartGracePeriod();
    domain.Quiescent(reader);
    if (!domain.IsComplete(token)) {
      state.SkipWithError("grace period did not complete");
      break;
    }
  }

  domain.Offline(reader);
  domain.Unregister(reader);
  domain.Drain();
}
BENCHMARK(BM_GracePeriodLatency);

}  // namespace
