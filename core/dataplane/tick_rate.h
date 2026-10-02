// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_TICK_RATE_H_
#define BESS_DATAPLANE_TICK_RATE_H_

#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>

// The conversion between wall-clock durations and the integer ticks that hot
// state keeps (roadmap M10, Decision D-053).
//
// A worker's clock is a free-running counter -- the TSC, or any other
// monotonic counter -- whose rate is known only at run time. The expiry wheel
// and the other dataplane code take ticks and never ask what a tick is, so
// the one place where seconds meet ticks is here, at the control boundary:
//
//   TickRate rate(tsc_hz);                          // pass it in; no global
//   uint64_t timeout = rate.Ticks(30s);             // config -> ticks
//   unsigned shift = rate.ShiftFor(1ms);            // wheel granularity
//
// The rules, all tested:
//   - Ticks() rounds down, never goes negative and saturates at the largest
//     tick count instead of wrapping, so a very long (or "forever") timeout
//     stays very long.
//   - Both directions are monotonic: a longer duration never converts to
//     fewer ticks.
//   - No arithmetic overflows: products use 128 bits.
//   - Nothing reads a clock. The caller reads its counter and passes ticks.

namespace bess::dataplane {

class TickRate {
 public:
  // ticks_per_second must be non-zero.
  constexpr explicit TickRate(uint64_t ticks_per_second) noexcept
      : tps_(ticks_per_second == 0 ? 1 : ticks_per_second) {}

  constexpr uint64_t per_second() const noexcept { return tps_; }

  // Whole ticks in `ns` nanoseconds, rounded down, saturating.
  constexpr uint64_t FromNanoseconds(uint64_t ns) const noexcept {
    const U128 ticks = static_cast<U128>(ns) * tps_ / kNanosPerSecond;
    return Saturate(ticks);
  }

  // Ticks in a duration; a negative duration is zero ticks.
  template <typename Rep, typename Period>
  constexpr uint64_t Ticks(std::chrono::duration<Rep, Period> d) const noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d);
    return ns.count() <= 0 ? 0 : FromNanoseconds(static_cast<uint64_t>(ns.count()));
  }

  // The duration `ticks` last, rounded down to a nanosecond and saturating at
  // the largest representable duration.
  constexpr std::chrono::nanoseconds ToDuration(uint64_t ticks) const noexcept {
    const U128 ns = static_cast<U128>(ticks) * kNanosPerSecond / tps_;
    constexpr U128 kMax = std::numeric_limits<int64_t>::max();
    return std::chrono::nanoseconds(
        static_cast<int64_t>(ns > kMax ? kMax : ns));
  }

  // The smallest `s` such that 2^s ticks last at least `granule` (at least 0).
  // ExpiryWheel's granularity shift for "about this coarse".
  constexpr unsigned ShiftFor(std::chrono::nanoseconds granule) const noexcept {
    const uint64_t ticks = Ticks(granule);
    return ticks <= 1 ? 0u
                      : static_cast<unsigned>(std::bit_width(ticks - 1));
  }

 private:
  __extension__ typedef unsigned __int128 U128;
  static constexpr U128 kNanosPerSecond = 1'000'000'000u;

  static constexpr uint64_t Saturate(U128 v) noexcept {
    constexpr U128 kMax = std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(v > kMax ? kMax : v);
  }

  uint64_t tps_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_TICK_RATE_H_
