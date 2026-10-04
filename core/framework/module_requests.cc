// SPDX-License-Identifier: BSD-3-Clause

#include "framework/module_requests.h"

#include <algorithm>

namespace bess::framework {

void RequestHub::Add(detail::RequestSlot *slot) { slots_.push_back(slot); }

void RequestHub::Remove(detail::RequestSlot *slot) noexcept {
  slots_.erase(std::remove(slots_.begin(), slots_.end(), slot), slots_.end());
}

size_t RequestHub::Deliver() {
  size_t delivered = 0;
  // A handler may open or close endpoints (a module command it triggers):
  // iterate over a copy, and skip slots removed meanwhile.
  const std::vector<detail::RequestSlot *> snapshot = slots_;
  for (detail::RequestSlot *slot : snapshot) {
    if (std::find(slots_.begin(), slots_.end(), slot) == slots_.end()) {
      continue;
    }
    delivered += slot->DeliverIfReady() ? 1 : 0;
  }
  return delivered;
}

}  // namespace bess::framework
