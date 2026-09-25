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

#ifndef BESS_DATAPLANE_BATCH_STAGES_H_
#define BESS_DATAPLANE_BATCH_STAGES_H_

#include <concepts>
#include <cstddef>

#include "dataplane/batch_tuning.h"

namespace bess::dataplane {

// Stage-major batch execution: the VPP-style "prefetch the whole batch, then
// work on it" shape, as an opt-in helper for module and table authors.
//
//   RunStages(n,
//             [&](size_t i) { h[i] = Hash(key[i]); table.Prefetch(h[i]); },
//             [&](size_t i) { out[i] = table.Find(h[i], key[i]); });
//
// runs the first stage over every element, then the second over every
// element, and so on. Written that way, the cache misses a stage starts
// (prefetches) are in flight together and have landed by the time the next
// stage touches the lines, instead of each element paying its misses in turn.
// Nothing else happens: no allocation, no type erasure, no indirect call --
// each stage is inlined into its own loop.
//
// When it pays (measured, MODERNIZATION.md "K4.6"): tables bigger than L1d
// whose lookups either walk dependent lines (a cuckoo bucket, then its entry;
// a meter id, then its state) or branch on the loaded data (which slot, which
// bucket, hit or miss) -- a mispredicted data-dependent branch squashes the
// speculative loads of the keys after it, so the core stops overlapping them
// on its own. Measured: ExactMatch and NAT cuckoo tables -18..-48%, L2Forward
// -18..-33%, WildcardMatch tuples up to -49%, meters ~3x beyond L3. When it
// does not: one independent, branch-free load per element (rte_lpm's tbl24),
// which the out-of-order core already overlaps (K7). ResolveLookupBody()
// encodes exactly that rule; nothing in BESS requires staging.
template <typename... Stages>
  requires(std::invocable<Stages &, size_t> && ...)
inline void RunStages(size_t n, Stages &&...stages) {
  (
      [&] {
        for (size_t i = 0; i < n; i++) {
          stages(i);
        }
      }(),
      ...);
}

// The same stages, with the batch body chosen at run time: kStaged runs them
// stage-major (RunStages); kPlain runs them element-major -- every stage for
// element 0, then element 1, ... -- which is the ordinary loop (a prefetch
// stage then touches a line the next stage uses immediately, at no real
// cost). Authors write the stages once; which body a table uses is decided
// at init by ResolveLookupBody() (batch_tuning.h) from its size and the host
// caches. One branch per batch; both bodies are fully inlined.
template <typename... Stages>
  requires(std::invocable<Stages &, size_t> && ...)
inline void RunBatch(LookupBody body, size_t n, Stages &&...stages) {
  if (body == LookupBody::kStaged) {
    RunStages(n, stages...);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    (stages(i), ...);
  }
}

// A typed prefetch hint. Intent matters on some CPUs (a write prefetch asks
// for the line exclusive, saving an upgrade when the stage will modify it);
// locality 3 keeps the line in all cache levels, which is what a batch that
// uses it a few hundred cycles later wants.
enum class PrefetchIntent : int { kRead = 0, kWrite = 1 };

template <PrefetchIntent kIntent = PrefetchIntent::kRead, int kLocality = 3>
  requires(kLocality >= 0 && kLocality <= 3)
inline void Prefetch(const void *address) noexcept {
  __builtin_prefetch(address, static_cast<int>(kIntent), kLocality);
}

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_BATCH_STAGES_H_
