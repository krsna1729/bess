// SPDX-License-Identifier: BSD-3-Clause

#include "framework/module_tick.h"

#include <algorithm>

namespace bess::framework {

void TickHub::Add(detail::TickSlot *slot) { slots_.push_back(slot); }

void TickHub::Remove(detail::TickSlot *slot) noexcept {
  slots_.erase(std::remove(slots_.begin(), slots_.end(), slot), slots_.end());
}

size_t TickHub::Run(Clock::duration slice, uint64_t tick) {
  size_t called = 0;
  // A callback may open endpoints and close others (a module command it
  // triggers): iterate over a copy and skip slots removed meanwhile.
  const std::vector<detail::TickSlot *> snapshot = slots_;
  for (detail::TickSlot *slot : snapshot) {
    if (std::find(slots_.begin(), slots_.end(), slot) == slots_.end()) {
      continue;
    }
    const Clock::time_point start = Clock::now();
    if (start < slot->next_due) {
      continue;
    }
    // Before the call, so the slot is not read after the callback returns.
    slot->next_due = start + slot->period;
    TickContext ctx(*this, start + slice, tick);
    slot->callback(ctx);
    calls_++;
    called++;
    if (Clock::now() - start > slice) {
      overruns_++;
    }
  }
  return called;
}

std::vector<PendingEvent> TickHub::TakeEvents() { return std::exchange(events_, {}); }

}  // namespace bess::framework
