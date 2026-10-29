// SPDX-License-Identifier: BSD-3-Clause

#include "stats/keyed_counters.h"

#include <memory>
#include <type_traits>

#include "utils/dpdk_memory.h"

namespace bess::stats {

KeyedCounters::KeyedCounters(size_t keys, size_t cells_per_key, size_t workers)
    : keys_(keys),
      cells_per_key_(cells_per_key),
      workers_(workers),
      words_per_plane_(RoundUpToLine(keys_ * cells_per_key_)),
      line_count_((words_per_plane_ / kWordsPerLine) * workers_),
      planes_(static_cast<Line *>(utils::DpdkAllocate(
          line_count_ * sizeof(Line),
          alignof(Line) > RTE_CACHE_LINE_SIZE ? alignof(Line) : RTE_CACHE_LINE_SIZE))) {
  promise(keys >= 1);
  promise(cells_per_key >= 1);
  promise(workers >= 1);
  promise(workers <= dataplane::kMaxWorkers);
  static_assert(std::is_nothrow_default_constructible_v<Line>);
  std::uninitialized_value_construct_n(planes_, line_count_);
}

KeyedCounters::~KeyedCounters() {
  std::destroy_n(planes_, line_count_);
  utils::DpdkFree(planes_);
}

}  // namespace bess::stats
