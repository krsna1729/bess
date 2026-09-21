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

#ifndef BESS_RCU_RCU_PTR_H_
#define BESS_RCU_RCU_PTR_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include <glog/logging.h>

#include "rcu/rcu_domain.h"

namespace bess {
namespace rcu {

// A published, immutable pointer to dataplane state (K1).
//
// Reader side -- one acquire load, nothing else:
//
//   const T *state = published_.Read();
//
// No shared_ptr, no refcount, no lock, no allocation and no destruction on the
// packet path. The pointer is valid until the worker reaches its next
// quiescent state, which is why a module uses it for one task invocation and
// does not cache it across invocations.
//
// Writer side -- the order matters and is enforced here:
//
//   fully build the replacement
//   release-store it into the atomic      (no new reader can acquire the old one)
//   start a grace period
//   retire the old object against it
//
// Starting the grace period *before* publishing would let a reader pass the
// grace period and then still acquire the old pointer, so `Publish()` does the
// store first and only then starts the grace period. Acquire/release handles
// publication visibility; QSBR handles lifetime -- they solve different
// problems and both are needed.
//
// Objects are immutable once published. Mutating published state and calling it
// safe because of QSBR is a misuse: mutable high-frequency state belongs in a
// different mechanism (worker-local state, K6).
//
// Writers are serialized by a plain mutex: RCU solves reader-vs-writer
// lifetime, not writer-vs-writer ordering, and callers are not assumed to hold
// any particular outer lock.
template <typename T>
class RcuPtr {
 public:
  explicit RcuPtr(RcuDomain &domain) : domain_(domain) {}

  RcuPtr(const RcuPtr &) = delete;
  RcuPtr &operator=(const RcuPtr &) = delete;

  ~RcuPtr() {
    // Destroying a published pointer while a reader could still hold it would
    // be a use-after-free. Teardown goes through ResetQuiesced() (workers
    // paused) or Publish() (readers running), never through the destructor.
    DCHECK(owner_ == nullptr || domain_.online_readers() == 0)
        << "RcuPtr destroyed while " << domain_.online_readers()
        << " reader(s) are online; clear it with ResetQuiesced() first";
  }

  // Installs the first object. Call it before readers can run (module Init),
  // when no reader can be holding a previous pointer.
  void Initialize(std::unique_ptr<const T> initial) {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    DCHECK(owner_ == nullptr) << "RcuPtr already initialized";
    owner_ = std::move(initial);
    current_.store(owner_.get(), std::memory_order_release);
  }

  // One acquire load per use. Valid until the current worker reaches its next
  // quiescent state; do not cache it beyond the invocation that read it.
  const T *Read() const noexcept {
    return current_.load(std::memory_order_acquire);
  }

  // Publishes `replacement` and retires the object it replaced against a fresh
  // grace period. Returns that grace period's token, so a caller publishing
  // several objects can see which token covered this one.
  //
  // This does *not* wait for the grace period: reclamation is deferred, and
  // whoever reclaims (a control thread) picks the object up once readers are
  // done with it.
  GracePeriod Publish(std::unique_ptr<const T> replacement) {
    std::lock_guard<std::mutex> lock(writer_mutex_);

    std::unique_ptr<const T> old = ExchangeLocked(std::move(replacement));
    const GracePeriod token = domain_.StartGracePeriod();
    domain_.Retire(token, std::move(old));
    return token;
  }

  // Publishes `replacement` and hands the previous object back to the caller
  // instead of retiring it. This is the primitive for publishing several
  // objects and retiring all of them against *one* grace period:
  //
  //   auto old_a = ptr_a.Exchange(std::move(new_a));
  //   auto old_b = ptr_b.Exchange(std::move(new_b));
  //   const auto token = domain.StartGracePeriod();
  //   domain.Retire(token, std::move(old_a));
  //   domain.Retire(token, std::move(old_b));
  //
  // The caller owns the returned object and must retire or destroy it.
  std::unique_ptr<const T> Exchange(std::unique_ptr<const T> replacement) {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    return ExchangeLocked(std::move(replacement));
  }

  // Drops the active object without waiting for readers, and returns it to the
  // caller. The name is the contract: the caller must guarantee that no reader
  // can hold the pointer -- the usual case is a structural transaction that has
  // already paused workers before destroying a module.
  std::unique_ptr<const T> ResetQuiesced() {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    current_.store(nullptr, std::memory_order_release);
    return std::move(owner_);
  }

  RcuDomain &domain() { return domain_; }

 private:
  std::unique_ptr<const T> ExchangeLocked(std::unique_ptr<const T> replacement) {
    // Publish first: after this store no new reader can acquire the object
    // being replaced. Everything the replacement's constructor wrote is
    // visible to a reader that observes this store.
    current_.store(replacement.get(), std::memory_order_release);
    return std::exchange(owner_, std::move(replacement));
  }

  RcuDomain &domain_;
  std::atomic<const T *> current_{nullptr};

  // Writer-side ownership of the active object only; retired objects belong to
  // the domain's retirement queue until a control thread reclaims them.
  std::unique_ptr<const T> owner_;
  std::mutex writer_mutex_;
};

}  // namespace rcu
}  // namespace bess

#endif  // BESS_RCU_RCU_PTR_H_
