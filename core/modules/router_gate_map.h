// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ROUTER_GATE_MAP_H_
#define BESS_MODULES_ROUTER_GATE_MAP_H_

#include <cstdint>
#include <limits>

#include "dataplane/interface_id.h"
#include "gate.h"

namespace bess::modules::router {

// The Router module's whole interface <-> gate mapping (D-049): interface n
// leaves on gate n - 1, and the invalid interface (0), or one beyond the gate
// space, drops. The unsigned subtraction folds both bounds into one compare.
// The route library holds interfaces only; nothing outside this module knows
// gates. In a header so that its per-packet cost can be measured on its own
// (modules/router_bench.cc).
inline gate_idx_t GateOf(dataplane::InterfaceId egress) {
  const uint32_t gate = static_cast<uint32_t>(egress.value()) - 1u;
  return gate < MAX_GATES ? static_cast<gate_idx_t>(gate) : DROP_GATE;
}

// The wire still names a gate (`egress_gate`); DROP_GATE means no interface.
// `gate` has passed IsValidGateValue().
static_assert(MAX_GATES < std::numeric_limits<uint16_t>::max(),
              "every gate has an interface (gate + 1)");
inline dataplane::InterfaceId InterfaceOf(uint64_t gate) {
  return gate == DROP_GATE
             ? dataplane::kInvalidInterfaceId
             : dataplane::InterfaceId(static_cast<uint16_t>(gate + 1));
}

}  // namespace bess::modules::router

#endif  // BESS_MODULES_ROUTER_GATE_MAP_H_
