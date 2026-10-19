// SPDX-License-Identifier: BSD-3-Clause
// The built-in fill-level watch (D-165). Experimental API.
//
// A controller that wants to know when a table is nearly full should not have
// to read every resource every second. The daemon already knows what each
// registered resource holds and, where the resource states one, its capacity
// (Resource::LiveCount and Resource::Capacity, GetResourceStatus, D-149), and
// its maintenance loop already runs control-side work on a clock. So it says
// so itself: a resource whose fill reaches the level is reported once, as one
// event on the log WatchEvents streams, and is reported again only after it
// falls below the level and rises again -- or after it stops being reachable
// and comes back, which is a new table with no history. A count that rises and
// falls between two sweeps is not seen at all: this samples, it does not watch
// continuously, which is why polling stays the backstop.
//
// The level is a fraction of the capacity the resource states, not a count:
// 80% means the same thing for a 512-slot table and a million-key map, and it
// survives a capacity that changes with a module's configuration. A resource
// that states no capacity is not watched, and that is not an error: it is the
// honest answer for a structure that grows with the workload, and reporting it
// as full would be a lie. A resource whose capacity is zero is not watched
// either: there is nothing for it to be full of.
//
// The sweep runs on the maintenance thread with the control-plane lock held,
// beside the modules' tick endpoints, and may block like them. The walk is a
// few hundred virtual calls, well inside the slice it is given; `stop()` (that
// slice) is checked every kStopCheckEvery resources, and a pass that runs out
// stops there and the next sweep starts again from the beginning. Visiting a
// resource twice is harmless: what has fired is remembered by name, and the
// state is small (the resources currently at or past the level). The limit of
// having no cursor is that a daemon whose resource count made a full walk
// exceed one slice would never reach the tail of it; a cursor is the change to
// make if a graph ever gets there.
//
//   // built once, beside the maintenance loop:
//   control::ResourceFillWatch fills(engine, bindings);
//   // the loop, under the control-plane lock:
//   control::ResourceFillWatch::Result r = fills.Sweep([this] { return slice_expired(); });
//   // after the lock, on the event log:
//   for (const auto &c : r.crossed) { events->Emit(kResourceFillEvent, ModuleOf(c.resource), {...}); }

#ifndef BESS_CONTROL_RESOURCE_FILL_H_
#define BESS_CONTROL_RESOURCE_FILL_H_

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dataplane/resource.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"

namespace bess::control {

// The event a fill crossing is reported as. Its source is the resource's
// module (the part of the name before the slash), its fields are "resource"
// (the full name), "count" (what the resource held) and "limit" (the count
// the level is at), and "capacity" (what the resource states) when it states
// one -- a resource watched at a level of its own may state none.
inline constexpr const char *kResourceFillEvent = "bess.resource_fill";

class ResourceFillWatch {
 public:
  // Resources between two looks at the stop condition.
  static constexpr size_t kStopCheckEvery = 32;
  // The fill a resource is reported at, as a percentage of what it states.
  // A table that is 80% full still has room to plan with; a report at 100%
  // would arrive with the writes it was meant to prevent.
  static constexpr uint64_t kLevelPercent = 80;

  struct Crossing {
    std::string resource;
    uint64_t count = 0;  // what the resource held
    uint64_t limit = 0;  // the count the level is at
    std::optional<uint64_t> capacity;  // what the resource states, if anything
  };
  struct Result {
    std::vector<Crossing> crossed;  // fired this sweep, and now remembered
    size_t visited = 0;             // resources looked at
    bool incomplete = false;        // the slice ran out: the rest is next sweep
  };

  ResourceFillWatch(dataplane::TransactionEngine &engine,
                    const framework::ResourceBindings &bindings)
      : engine_(engine), bindings_(bindings) {}

  // The level is a percentage of the stated capacity, so this is the base the
  // arithmetic below rounds in.
  static constexpr uint64_t kPercentBase = 100;

  // The smallest count that reaches the level: ceil(capacity * kLevelPercent
  // / 100). Floor would put the threshold one below the level for any capacity
  // that is not a multiple of 5 -- 512 fires at 409, which is 79.9% -- and
  // would make a capacity of 1 reach the level at 0, so a module's settings
  // singleton (capacity 1, LiveCount always 0, singleton_resource.h) would
  // report a crossing while empty and never fall below it. The remainder term
  // is at most kLevelPercent + kPercentBase - 1, so nothing overflows.
  static constexpr uint64_t LevelOf(uint64_t capacity) noexcept {
    return capacity / kPercentBase * kLevelPercent +
           (capacity % kPercentBase * kLevelPercent + kPercentBase - 1) / kPercentBase;
  }

  // Visits every reachable resource once, firing for each one that is at or
  // past its level and forgetting one that has fallen below it. `stop` is
  // asked whether the tick's slice is spent, every kStopCheckEvery resources.
  template <typename Stop>
  Result Sweep(Stop &&stop) {
    Result result;
    seen_.clear();
    for (const std::string &name : engine_.ResourceNames()) {
      if (result.visited != 0 && result.visited % kStopCheckEvery == 0 && stop()) {
        result.incomplete = true;
        return result;
      }
      const dataplane::Resource *resource = engine_.FindResource(name);
      if (resource == nullptr || bindings_.Find(*resource) == nullptr) {
        continue;  // not reachable, as GetResourceStatus and List have it
      }
      result.visited++;
      seen_.insert(name);
      // The level is the resource's own count when it states one (a module
      // knows its table), and otherwise a fraction of the capacity it states.
      // A resource that states neither is not watched: D-149's "not stated"
      // is the honest answer for a table with no number to give. A stated
      // level of zero is read as none: it would fire for an empty resource and
      // never fall below it, so it would sit fired for the daemon's life.
      const std::optional<uint64_t> stated = resource->FillLevel();
      const std::optional<uint64_t> capacity = resource->Capacity();
      uint64_t limit;
      if (stated && *stated != 0) {
        limit = *stated;
      } else if (capacity && *capacity != 0) {
        limit = LevelOf(*capacity);
      } else {
        fired_.erase(name);  // nothing to watch it against
        continue;
      }
      const uint64_t count = resource->LiveCount();
      if (count < limit) {
        fired_.erase(name);  // below the level again: it may fire once more
        continue;
      }
      if (fired_.insert(name).second) {
        result.crossed.push_back({name, count, limit, capacity});
      }
    }
    // A pass that saw every resource also saw which names are gone (a module
    // unloaded, a table unregistered). Forgetting them here keeps a later
    // resource of the same name from arriving already fired, and keeps the
    // state bounded by the resources that exist rather than by every name
    // that has ever fired. A pass the slice cut short forgets nothing: it did
    // not see the resources it never reached.
    for (auto it = fired_.begin(); it != fired_.end();) {
      it = seen_.contains(*it) ? std::next(it) : fired_.erase(it);
    }
    return result;
  }

  // The resources currently at or past their level, for a test or a report.
  size_t fired() const noexcept { return fired_.size(); }

 private:
  dataplane::TransactionEngine &engine_;
  const framework::ResourceBindings &bindings_;
  // The names at or past their level (fired once, until they fall below), and
  // the names the current pass has seen -- a pass that reaches every resource
  // uses it to forget the names that are gone.
  std::unordered_set<std::string> fired_;
  std::unordered_set<std::string> seen_;
};

// The module a resource belongs to: the part of "<module>/<table>" before the
// slash, or the whole name when it has none (a name a module chose itself).
inline std::string ModuleOf(const std::string &resource) {
  const size_t slash = resource.find('/');
  return slash == std::string::npos ? resource : resource.substr(0, slash);
}

}  // namespace bess::control

#endif  // BESS_CONTROL_RESOURCE_FILL_H_
