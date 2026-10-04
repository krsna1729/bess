// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_TESTING_ALLOCATION_FAULTS_H_
#define BESS_TESTING_ALLOCATION_FAULTS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

// Deterministic allocation counting and fail-Nth injection for tests (M22).
//
// testing/allocation_faults.cc replaces every replaceable global operator new
// and delete (plain, array, nothrow, aligned, aligned nothrow, sized and
// aligned delete): all of them, so that under AddressSanitizer no block is
// allocated by the runtime's operator new and freed by ours
// (alloc-dealloc-mismatch). A replacement has to be in the executable, so a
// test opts in by linking that object (core/meson.build lists the tests);
// production binaries never contain it.
//
// While an AllocationFaults window is open, the allocations it sees are
// counted from 0, and the one numbered `fail_at` is refused: the throwing
// forms throw std::bad_alloc, the nothrow forms return nullptr. Every later
// allocation succeeds. By default a window sees only the thread that opened
// it (other threads -- a reader, a reclaimer -- allocate freely); kAllThreads
// sees every thread. Windows on one thread nest: the innermost one counts.
//
// The "failure at every point leaves no trace" loop:
//
//   const size_t points = ForEachFailurePoint([&](size_t k) {
//     ...set up, snapshot the observable state...
//     bool threw = false;
//     {
//       AllocationFaults faults(k);
//       try { operation(); } catch (const std::bad_alloc &) { threw = true; }
//       if (!faults.injected()) { ...the operation succeeded... ; return; }
//     }
//     ...state equals the snapshot; the operation succeeds on retry...
//   });
//
// ForEachFailurePoint runs fn(k) for k = 0, 1, 2, ... until a run in which no
// allocation was refused (every allocation the operation makes has had its
// turn to fail) and returns that k: the number of failure points exercised.
namespace bess::fault_injection {

inline constexpr size_t kNoFailure = std::numeric_limits<size_t>::max();

namespace internal {

struct Window {
  std::atomic<size_t> allocations{0};
  std::atomic<size_t> frees{0};
  size_t fail_at = kNoFailure;
  std::atomic<bool> injected{false};
};

// The opening thread's innermost window, or null.
Window *ExchangeThreadWindow(Window *window) noexcept;
// The one all-threads window: static storage, so a thread that read the
// "open" flag never touches a destroyed object.
Window &SharedWindow() noexcept;
void OpenSharedWindow(size_t fail_at) noexcept;
void CloseSharedWindow() noexcept;

}  // namespace internal

// How many allocations have been refused in this process, by any window.
size_t InjectedFailures() noexcept;

class AllocationFaults {
 public:
  enum class Threads : uint8_t { kThisThread, kAllThreads };

  // Counts allocations; refuses the `fail_at`-th (0-based), or none.
  explicit AllocationFaults(size_t fail_at = kNoFailure,
                            Threads threads = Threads::kThisThread)
      : threads_(threads) {
    if (threads_ == Threads::kAllThreads) {
      internal::OpenSharedWindow(fail_at);
    } else {
      own_.fail_at = fail_at;
      previous_ = internal::ExchangeThreadWindow(&own_);
    }
  }
  ~AllocationFaults() {
    if (threads_ == Threads::kAllThreads) {
      internal::CloseSharedWindow();
    } else {
      (void)internal::ExchangeThreadWindow(previous_);
    }
  }
  AllocationFaults(const AllocationFaults &) = delete;
  AllocationFaults &operator=(const AllocationFaults &) = delete;

  // Allocation attempts seen, the refused one included.
  size_t allocations() const noexcept {
    return window().allocations.load(std::memory_order_relaxed);
  }
  // Non-null pointers freed while the window was open (allocated in it or not).
  size_t frees() const noexcept { return window().frees.load(std::memory_order_relaxed); }
  // The `fail_at`-th allocation happened and was refused.
  bool injected() const noexcept { return window().injected.load(std::memory_order_relaxed); }

 private:
  const internal::Window &window() const noexcept {
    return threads_ == Threads::kAllThreads ? internal::SharedWindow() : own_;
  }

  Threads threads_;
  internal::Window own_;
  internal::Window *previous_ = nullptr;
};

namespace internal {

// The current test's result-part count, and whether a part from `since` on
// is a fatal failure (an ASSERT_* that returned from fn).
inline int TestPartCount() {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  return info == nullptr ? 0 : info->result()->total_part_count();
}
inline bool FatalFailureSince(int since) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  if (info == nullptr) {
    return ::testing::Test::HasFatalFailure();
  }
  const ::testing::TestResult *result = info->result();
  for (int i = since; i < result->total_part_count(); i++) {
    if (result->GetTestPartResult(i).fatally_failed()) {
      return true;
    }
  }
  return false;
}

}  // namespace internal

// Runs fn(k) for k = 0, 1, 2, ... until a run refuses no allocation, and
// returns that k. fn opens an AllocationFaults(k) window around the operation
// under test. Stops early when fn(k) fails fatally (an ASSERT_* in it), and
// fails the test past `limit` points (an operation that allocates without
// bound).
template <typename Fn>
size_t ForEachFailurePoint(Fn &&fn, size_t limit = size_t{1} << 16) {
  for (size_t k = 0; k <= limit; k++) {
    const size_t injected_before = InjectedFailures();
    const int parts_before = internal::TestPartCount();
    fn(k);
    if (internal::FatalFailureSince(parts_before) || InjectedFailures() == injected_before) {
      return k;
    }
  }
  ADD_FAILURE() << "an allocation was still refused at failure point " << limit;
  return limit;
}

}  // namespace bess::fault_injection

#endif  // BESS_TESTING_ALLOCATION_FAULTS_H_
