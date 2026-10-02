// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_FLOW_OBSERVER_H_
#define BESS_FLOW_FLOW_OBSERVER_H_

#include <concepts>
#include <cstdint>

#include "flow/flow_types.h"

namespace bess::flow {

// The seam between a flow table and whatever watches its flows' lifetime:
// the expiry engine of milestone M10, hit and miss counters, an offload
// agent. Decision D-052.
//
// The table calls the observer inline (it is a template parameter, so there
// is no virtual call and no std::function), from the thread that mutates the
// table, after the table's own bookkeeping for the event is consistent:
//
//   OnCreate(handle, state)  a flow now exists and is findable. Schedule its
//                            expiry here; keep the handle, not the state.
//   OnErase(handle, state)   a flow is about to go: its state is still alive,
//                            its keys are about to leave the index. Cancel
//                            the expiry record here.
//   OnFull()                 an Emplace was refused because no slot is free.
//
// The reverse direction needs nothing from this interface. An expiry engine
// keeps FlowHandles in its records; when a deadline passes it calls
// `table.Erase(handle)`. The handle carries the slot's generation, so a
// record that outlived its flow cannot erase the newer flow that reused the
// slot: Erase(handle) returns false. Refreshing a flow's deadline is a plain
// store in the application's State (M10 decides where), so the table has no
// Touch().
//
// An observer must not call a mutating operation of the table that notified
// it (re-entrancy is not supported). It runs on the table's hot create/erase
// path and must not block.
template <typename O, typename State>
concept FlowObserver = requires(O &observer, FlowHandle handle, State &state) {
  { observer.OnCreate(handle, state) } noexcept;
  { observer.OnErase(handle, state) } noexcept;
  { observer.OnFull() } noexcept;
};

// The default: nothing, and no storage.
struct NoFlowObserver {
  template <typename State>
  void OnCreate(FlowHandle, State &) noexcept {}
  template <typename State>
  void OnErase(FlowHandle, State &) noexcept {}
  void OnFull() noexcept {}
};

// Optional plain counters, for a table whose owner wants them. A
// worker-owned table's observer runs on the owner, so these are ordinary
// increments; a shared table calls its observer under the writer lock.
struct FlowCounters {
  uint64_t created = 0;
  uint64_t erased = 0;
  uint64_t rejected_full = 0;

  template <typename State>
  void OnCreate(FlowHandle, State &) noexcept {
    created++;
  }
  template <typename State>
  void OnErase(FlowHandle, State &) noexcept {
    erased++;
  }
  void OnFull() noexcept { rejected_full++; }
};

}  // namespace bess::flow

#endif  // BESS_FLOW_FLOW_OBSERVER_H_
