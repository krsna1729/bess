// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_INTERFACE_ID_H_
#define BESS_DATAPLANE_INTERFACE_ID_H_

#include <cstdint>
#include <type_traits>

#include "dataplane/strong_id.h"

namespace bess::dataplane {

// Names a logical forwarding endpoint: where a networking library says a packet
// should leave, without saying how a particular runtime reaches it. In a module
// graph an adapter maps it to an output gate; a fused appliance maps it to a
// port and queue; a hardware path maps it to a device action. Libraries hold
// `InterfaceId`, never `gate_idx_t` (roadmap M7, Decision D-049).
//
// Width and range: 32 bits, trivially copyable, no implicit conversion from or
// to an integer or another id. Zero is the invalid interface ("no endpoint":
// the packet is dropped by whatever maps it), as for `ActionId` and
// `NextHopId`. The adapter, not this type, bounds the valid range.
struct InterfaceIdTag;
using InterfaceId = StrongId<InterfaceIdTag, uint32_t>;
inline constexpr InterfaceId kInvalidInterfaceId{};

static_assert(sizeof(InterfaceId) == sizeof(uint32_t));
static_assert(std::is_trivially_copyable_v<InterfaceId>);
static_assert(std::is_standard_layout_v<InterfaceId>);
static_assert(!std::is_convertible_v<uint32_t, InterfaceId>,
              "an integer must not convert to an InterfaceId implicitly");
static_assert(!std::is_convertible_v<InterfaceId, uint32_t>,
              "an InterfaceId must not convert to an integer implicitly");

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_INTERFACE_ID_H_
