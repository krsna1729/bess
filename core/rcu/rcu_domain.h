// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RCU_RCU_DOMAIN_H_
#define BESS_RCU_RCU_DOMAIN_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rte_rcu_qsbr.h>

namespace bess {
namespace rcu {

// The reader identity is the BESS WorkerId, not a CPU, a DPDK lcore or a thread
// id: it survives worker pause/resume, DPDK thread registration, and worker
// destruction/recreation (MODERNIZATION.md section 10, K1).
using ReaderId = uint32_t;
using GracePeriod = uint64_t;

struct RcuStats {
  uint64_t grace_periods_started = 0;
  uint64_t grace_periods_completed = 0;
  uint64_t objects_retired = 0;
  uint64_t objects_reclaimed = 0;
  size_t pending_retired_objects = 0;
  GracePeriod oldest_pending_token = 0;
};

// RCU owns reader lifecycle errors; control code translates them at its
// boundary rather than pulling control types into a dataplane primitive.
struct ReaderRegistrationError {
  int err;
  std::string message;
};

// One dataplane reader domain per runtime (not one per module, table or
// classifier): workers register once, report quiescence periodically, and every
// immutable object publishes and retires through the same domain.
//
// The semantics are BESS's; DPDK's `rte_rcu_qsbr` is the backend, and no BESS
// code outside this directory is expected to know that.
//
// Two rules decide whether this is used correctly:
//
//  1. a reader must be *offline* before it blocks -- otherwise a grace period
//     waits for a thread that will not report quiescence until it is woken;
//  2. a writer publishes the replacement *before* starting the grace period
//     that retires the old object -- otherwise a reader can pass the grace
//     period and then still acquire the old pointer.
//
// Quiescence is a worker execution-state property: a worker is quiescent once a
// scheduled task invocation has returned to the scheduler and before the next
// one begins. It is never reported from inside packet processing.
class RcuDomain {
 public:
  // A pathological update rate must not grow the retirement queue without
  // bound. Past this many pending objects the *control* side reclaims what it
  // can and, if that is not enough, waits for outstanding grace periods --
  // never the packet path.
  static constexpr size_t kDefaultRetireHighWater = 4096;

  explicit RcuDomain(uint32_t max_readers,
                     size_t retire_high_water = kDefaultRetireHighWater);
  ~RcuDomain();

  RcuDomain(const RcuDomain &) = delete;
  RcuDomain &operator=(const RcuDomain &) = delete;

  uint32_t max_readers() const { return max_readers_; }

  // -- reader lifecycle -----------------------------------------------------

  // Registers a reader slot. A worker registers when its thread starts, stays
  // offline until dataplane execution is about to resume, and unregisters when
  // it is done. Registering an already-registered id is an error; registering a
  // previously unregistered id is how a recreated worker comes back.
  std::expected<void, ReaderRegistrationError> Register(ReaderId id);
  void Unregister(ReaderId id);

  // Online readers participate in grace periods; offline readers do not, which
  // is what lets a paused worker stop blocking reclamation.
  void Online(ReaderId id);
  void Offline(ReaderId id);
  bool IsOnline(ReaderId id) const;
  bool IsRegistered(ReaderId id) const;
  size_t registered_readers() const;
  size_t online_readers() const;

  // Reports a quiescent state for `id`. Call it only at a safe boundary: after
  // a task invocation returned, before the next one starts.
  void Quiescent(ReaderId id);

  // -- grace periods --------------------------------------------------------

  // Starts a grace period and returns its token. Writers call this *after*
  // publishing replacements, then retire the old objects against the token.
  GracePeriod StartGracePeriod();

  // Non-blocking: has every online reader passed a quiescent state since
  // `token` was started?
  bool IsComplete(GracePeriod token) const;

  // Blocks until the current grace period completes. Control side only.
  void Synchronize();

  // -- retirement -----------------------------------------------------------

  // Hands an object to the domain, to be destroyed once `token` completes. The
  // queue holds heterogeneous types: destruction goes through the type-erased
  // deleter captured here, and always runs on whichever control thread calls
  // ReclaimReady()/Drain() -- never on a packet worker.
  //
  // `outstanding`, if given, is incremented now and decremented right after
  // the object is destroyed: an owner (a transaction resource, a plugin) can
  // tell when the last destructor it supplied has run -- the deleter is code
  // in the owner's module, so the owner must not go away before.
  template <typename T>
  void Retire(GracePeriod token, std::unique_ptr<T> &&object,
              std::atomic<size_t> *outstanding = nullptr) {
    if (object == nullptr) {
      return;
    }
    // The type-erased queue holds a plain void*; the deleter remembers T, so
    // both `unique_ptr<T>` and `unique_ptr<const T>` work.
    void *raw = const_cast<void *>(static_cast<const void *>(object.release()));
    RetireErased(token, raw, [](void *p) { delete static_cast<T *>(p); },
                 outstanding);
  }

  // Makes room for `n` more retirements, so that the next `n` Retire() calls
  // do not allocate (for callers that must not fail after committing).
  void ReserveRetirements(size_t n);

  // Destroys every retired object whose grace period has completed, and returns
  // how many were destroyed. Call from a control thread.
  size_t ReclaimReady();

  // Waits for all outstanding grace periods and reclaims everything. For
  // teardown, not for the normal update path.
  void Drain();

  RcuStats Stats() const;

  // Past this many pending objects, RetireErased() reclaims and then waits
  // for grace periods (control side). Callers that must never wait, such as
  // the transaction engine, stay below it.
  size_t retire_high_water() const { return retire_high_water_; }

  // Objects retired and not yet reclaimed, without taking a lock (an
  // approximate, recent value; for backpressure decisions).
  size_t pending_retired() const {
    return pending_count_.load(std::memory_order_relaxed);
  }

  // The underlying DPDK QSBR variable, for DPDK libraries with native QSBR
  // integration (K7: `rte_lpm_rcu_qsbr_add`), so their internal reclamation
  // waits on the same worker quiescent states as everything else. Hand it to
  // such a library only; BESS code uses the semantic API above.
  struct rte_rcu_qsbr *dpdk_qsbr() const noexcept { return qsbr_; }

 private:
  struct RetiredObject {
    GracePeriod token;
    void *object;
    void (*destroy)(void *);
    std::atomic<size_t> *outstanding;  // decremented after destroy, or null
  };

  void RetireErased(GracePeriod token, void *object, void (*destroy)(void *),
                    std::atomic<size_t> *outstanding = nullptr);

  // Lock-free, stats-free grace-period check: safe to call while holding
  // retire_mutex_ (which ReclaimReady does).
  bool IsGracePeriodComplete(GracePeriod token) const;

  void *qsbr_mem_ = nullptr;
  struct rte_rcu_qsbr *qsbr_ = nullptr;
  const uint32_t max_readers_;

  mutable std::mutex state_mutex_;
  std::vector<uint8_t> registered_;
  std::vector<uint8_t> online_;
  size_t registered_readers_ = 0;

  mutable std::mutex retire_mutex_;
  // In retirement order, from retired_[head_]. Tokens come from
  // StartGracePeriod() in increasing order and DPDK completes them in order,
  // so reclamation advances head_ and stops at the first incomplete token:
  // O(reclaimed), not O(pending). An object retired against an older token
  // behind a newer one is only freed later, never earlier. A vector (compacted
  // in place) rather than a deque, so that ReserveRetirements() can guarantee
  // later pushes do not allocate.
  std::vector<RetiredObject> retired_;
  size_t head_ = 0;
  std::atomic<size_t> pending_count_{0};  // retired_.size(), readable unlocked
  const size_t retire_high_water_;
  mutable RcuStats stats_;  // counters are updated by const readers too
};

}  // namespace rcu
}  // namespace bess

#endif  // BESS_RCU_RCU_DOMAIN_H_
