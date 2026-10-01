// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_GENERATION_HANDLE_H_
#define BESS_DATAPLANE_GENERATION_HANDLE_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace bess::dataplane {

// An id plus the generation of the slot it named when the handle was issued
// (roadmap M7; handle rules in docs/architecture.md section 7).
//
// A plain `StrongId` is enough for a synchronous RCU reader: it resolves the id
// and uses the object before its next quiescent state. A handle that outlives
// that window -- one parked in a punt queue, a continuation, or a hardware
// flow's completion -- can meet a slot that has since been freed and reused. It
// carries the generation so the owner can see the mismatch and fail closed
// instead of acting on the new object. The owner compares `generation` with the
// slot's current one; this type only carries and compares them.
//
// `Id` is a 32-bit identifier, so a handle is eight bytes with no padding and
// compares as one 64-bit word (a member-wise compare costs a branch).
template <typename Id>
struct GenerationHandle {
  static_assert(sizeof(Id) == sizeof(uint32_t),
                "a handle's id is 32 bits, so the handle is one 64-bit word");

  Id id{};
  uint32_t generation = 0;

  friend constexpr bool operator==(const GenerationHandle &a,
                                   const GenerationHandle &b) {
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
  }
};

template <typename Id>
struct GenerationHandleHash {
  size_t operator()(const GenerationHandle<Id> &handle) const noexcept {
    // Both halves identify the object; neither alone does.
    return std::hash<uint64_t>{}(std::bit_cast<uint64_t>(handle));
  }
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_GENERATION_HANDLE_H_
