// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_FLOW_TYPES_H_
#define BESS_FLOW_FLOW_TYPES_H_

#include <cstddef>
#include <cstdint>

#include "dataplane/generation_handle.h"
#include "dataplane/strong_id.h"

// Vocabulary shared by the flow-state tables (roadmap M9, Decision D-052).
// Experimental API: installed, but may change without source compatibility.

namespace bess::flow {

// Names one live flow's state slot. Index-like and one-based: zero is "no
// flow". A bare FlowId is enough for a synchronous reader (it resolves the id
// and uses the state before its next quiescent state); an id that outlives
// that window travels as a FlowHandle.
struct FlowIdTag;
using FlowId = dataplane::StrongId<FlowIdTag, uint32_t>;

// A FlowId plus the generation of the slot it named when it was issued. It
// resolves only while that flow is alive: once the flow is erased the handle
// stays dead even after the slot holds a different flow (architecture.md
// section 7). Eight bytes, one 64-bit compare.
using FlowHandle = dataplane::GenerationHandle<FlowId>;

// The handle of no flow. FlowHandle{} compares equal to it.
inline constexpr FlowHandle kNoFlow{};

// Which free slot a new flow takes. The generation protects a stale handle
// either way; the policy decides how soon a slot is reused.
enum class SlotReuse : uint8_t {
  // The slot freed most recently: its state and key are still warm in cache.
  // The same few slots turn over under churn, so a long-lived handle sees the
  // most generations.
  kLifo,
  // The slot freed longest ago: slots turn over evenly, so a slot's
  // generation advances 1/capacity as fast. Use when handles live long
  // (queued continuations, hardware flow marks).
  kFifo,
};

enum class EmplaceStatus : uint8_t {
  kCreated,      // a new flow now exists
  kExists,       // the key was already present; nothing changed
  kFull,         // no free slot; nothing changed (see docs/flow-state.md)
  kAliasExists,  // EmplaceAliased: the alias key was already present; nothing
                 // changed
};

enum class AliasStatus : uint8_t {
  kAdded,
  kStale,   // the handle does not name a live flow
  kExists,  // the alias key is already in the table (for any flow)
  kNoRoom,  // the flow already has Traits::kAliases aliases
};

template <typename State>
struct EmplaceResult {
  State *state = nullptr;  // the new flow's state, or the existing flow's
  FlowHandle handle{};
  EmplaceStatus status = EmplaceStatus::kFull;

  bool created() const noexcept { return status == EmplaceStatus::kCreated; }
  explicit operator bool() const noexcept { return state != nullptr; }
};

// What a key lookup found.
template <typename State>
struct FlowRef {
  State *state = nullptr;  // nullptr: the key is absent
  FlowHandle handle{};
  bool via_alias = false;  // the key was one of the flow's aliases

  explicit operator bool() const noexcept { return state != nullptr; }
};

// Why a table could not be created. Construction is the only place a table
// allocates.
enum class FlowTableError : uint8_t {
  kInvalidCapacity,  // zero, or more flows/keys than 32-bit ids can name
  kTooLarge,         // the memory size overflows size_t
  kOutOfMemory,      // the allocator refused
  kBackendFailed,    // the shared directory could not be created
};

inline const char *ToString(FlowTableError error) noexcept {
  switch (error) {
    case FlowTableError::kInvalidCapacity:
      return "invalid flow table capacity";
    case FlowTableError::kTooLarge:
      return "flow table size overflows";
    case FlowTableError::kOutOfMemory:
      return "out of memory creating the flow table";
    case FlowTableError::kBackendFailed:
      return "shared flow directory could not be created";
  }
  return "unknown flow table error";
}

}  // namespace bess::flow

#endif  // BESS_FLOW_FLOW_TYPES_H_
