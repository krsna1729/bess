// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_NEXT_HOP_ID_H_
#define BESS_ROUTE_NEXT_HOP_ID_H_

#include <cstdint>

#include "dataplane/strong_id.h"

namespace bess::route {

// Next-hop ids are one-based, with zero invalid; the route table stores 24 bits.
struct NextHopIdTag;
using NextHopId = bess::dataplane::StrongId<NextHopIdTag, uint32_t>;
inline constexpr NextHopId kInvalidNextHopId{};

}  // namespace bess::route

#endif  // BESS_ROUTE_NEXT_HOP_ID_H_
