// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_OWNER_H_
#define BESS_FLOW_OWNER_H_

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "dataplane/worker_id.h"

// Worker-ownership diagnostics for flow tables (roadmap M9).
//
// A worker-owned table has no synchronisation at all, so touching it from a
// second thread is a data race that nothing else will report. The table can
// check, in builds that ask for it, that every call comes from its owner.
// The owner is an opaque OwnerToken because the flow library sits below the
// worker: it may not include worker.h (tools/check_includes.py), so it cannot
// ask "which worker am I?" itself. The application injects that answer as an
// Owner policy type, typically
//
//   struct WorkerOwner {
//     static constexpr bool kChecked = true;      // or false in release
//     static OwnerToken Current() noexcept {
//       return flow::TokenOf(bess::CurrentWorkerId());  // stats/current_worker.h
//     }
//   };
//
// A policy with kChecked == false costs nothing: the token, the comparison and
// the call are not compiled.

namespace bess::flow {

using OwnerToken = uint64_t;

// No owner yet: the first call from any context binds the table to it.
inline constexpr OwnerToken kNoOwner = 0;

// The token for a worker. Never kNoOwner.
constexpr OwnerToken TokenOf(dataplane::WorkerId worker) noexcept {
  return OwnerToken{worker.value()} + 1;
}

template <typename P>
concept OwnerPolicy = requires {
  { P::kChecked } -> std::convertible_to<bool>;
  { P::Current() } noexcept -> std::same_as<OwnerToken>;
};

// Identifies the calling thread. Default for tables not tied to a bess worker
// (tests, tools, a control-side table).
struct ThreadOwner {
  static constexpr bool kChecked = true;
  static OwnerToken Current() noexcept;
};

// No ownership checking: nothing is stored or compiled.
struct UncheckedOwner {
  static constexpr bool kChecked = false;
  static OwnerToken Current() noexcept { return kNoOwner; }
};

// Debug builds check the calling thread; release builds check nothing. The
// choice changes a table's layout, so bessd and every plugin must agree: the
// build passes BESS_FLOW_OWNER_CHECKS (0 or 1) to bessd and in bess-dev's
// cflags, from bessd's build type, whatever a plugin's own NDEBUG. Without it
// (a consumer of the bare headers), NDEBUG decides, as before.
#if defined(BESS_FLOW_OWNER_CHECKS)
#if BESS_FLOW_OWNER_CHECKS
using DefaultOwner = ThreadOwner;
#else
using DefaultOwner = UncheckedOwner;
#endif
#elif defined(NDEBUG)
using DefaultOwner = UncheckedOwner;
#else
using DefaultOwner = ThreadOwner;
#endif

namespace detail {

// Reports a violation and aborts (LOG(FATAL)). Out of line so the table's hot
// code carries one cold call, not the logging machinery.
[[noreturn]] void OwnerViolation(const char *table_op, OwnerToken owner,
                                 OwnerToken caller);

}  // namespace detail

// The owner a table remembers. Unchecked policies make this an empty class.
template <OwnerPolicy Policy>
class OwnerGuard {
 public:
  // Called at the top of every operation. Binds the first caller; aborts when
  // a different context calls afterwards.
  void Check(const char *op) const {
    if constexpr (Policy::kChecked) {
      const OwnerToken caller = Policy::Current();
      if (owner_ == kNoOwner) [[unlikely]] {
        owner_ = caller;
      } else if (owner_ != caller) [[unlikely]] {
        detail::OwnerViolation(op, owner_, caller);
      }
    }
  }

  // Hands the table over: the next caller becomes the owner. Only the current
  // owner (or nobody) may release.
  void Release(const char *op) const {
    if constexpr (Policy::kChecked) {
      Check(op);
      owner_ = kNoOwner;
    }
  }

  OwnerToken owner() const noexcept {
    if constexpr (Policy::kChecked) {
      return owner_;
    } else {
      return kNoOwner;
    }
  }

 private:
  struct Empty {};
  [[no_unique_address]] mutable std::conditional_t<Policy::kChecked,
                                                   OwnerToken, Empty>
      owner_{};
};

}  // namespace bess::flow

#endif  // BESS_FLOW_OWNER_H_
