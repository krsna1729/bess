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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#ifndef BESS_UTILS_PUBLISHED_GENERATION_H_
#define BESS_UTILS_PUBLISHED_GENERATION_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace bess {
namespace utils {

// A published, immutable generation of some table, plus the writer protocol for
// replacing it while readers are running (MODERNIZATION.md Phase J).
//
// Readers call Snapshot() once per batch and hold the result for as long as
// they need the table; the generation stays alive until the last reader drops
// it. Writers call Update(), which asks the caller's builder for a replacement,
// publishes it, and then waits for the readers of the retired generation to
// drain -- so the retired generation is destroyed by the writer (control-plane)
// thread, not by whichever packet worker happens to finish last. That matters
// when destruction is expensive or must not happen on the data path: IPLookup's
// generation frees a 64MB `rte_lpm` under DPDK's global tailq lock, ExactMatch's
// frees a cuckoo table.
//
// Deliberately *not* here: how a generation is built. Replaying routes into an
// `rte_lpm`, or canonical rules plus resolved field specs into an
// `ExactMatchTable`, stays in the module. Keeping construction out means a
// later QSBR backend can replace this class without forcing the two modules into
// one table-building model.
template <typename Generation>
class PublishedGeneration {
 public:
  using Ptr = std::shared_ptr<const Generation>;

  // Installs the first generation. Call during Init(), before any batch can
  // snapshot; every later change goes through Update(), which is the only path
  // that honors the drain protocol.
  void Initialize(Ptr gen) {
    generation_.store(std::move(gen), std::memory_order_release);
  }

  // Drops the current generation without waiting for readers. Only valid when
  // no reader can be running: BESS pauses workers before deleting a module, and
  // this exists for exactly that teardown path. If readers can be live, use
  // Update() instead.
  void ResetQuiesced() {
    generation_.store(nullptr, std::memory_order_release);
  }

  // One acquisition per batch. Hold the result for the batch's duration; it
  // keeps the generation alive against a concurrent Update().
  Ptr Snapshot() const {
    return generation_.load(std::memory_order_acquire);
  }

  // Serializes writers, builds a replacement with `build`, publishes it, and
  // drains the readers of the retired generation. Returns false if `build`
  // returned nullptr, in which case the installed generation is untouched and
  // the builder owns reporting why (it can capture whatever error state its
  // module uses).
  //
  // `build` receives a *reference* on purpose: a builder that kept a
  // shared_ptr would hold a reference the drain loop then waits on forever.
  template <typename BuildFn>
  bool Update(const BuildFn &build) {
    std::lock_guard<std::mutex> guard(mutation_lock_);
    const Ptr current = generation_.load(std::memory_order_acquire);

    Ptr next = build(*current);
    if (next == nullptr) {
      return false;
    }

    // Publish first: after this store, no new batch can acquire `current`, so
    // only batches that snapshotted it before publication can still hold it.
    generation_.store(std::move(next), std::memory_order_release);

    // Then wait for those to drain, so that *this* thread drops the last
    // reference and therefore runs the retired generation's destructor. The
    // count only decreases once the atomic no longer points at `current` (no
    // new references can be taken), so a stale read can only delay this loop by
    // an iteration, never livelock it. Running under the writer lock also keeps
    // rapid commands from retiring several generations at once.
    while (current.use_count() != 1) {
      std::this_thread::yield();
    }
    return true;
  }

 private:
  // Never null between Init() and teardown; see Store().
  std::atomic<Ptr> generation_;
  std::mutex mutation_lock_;  // serializes Update()
};

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_PUBLISHED_GENERATION_H_
