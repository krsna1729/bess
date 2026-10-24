// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_RULE_RESULT_H_
#define BESS_CLASSIFIER_RULE_RESULT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gate.h"

namespace bess::classifier {

// One match rule's result: the gate the packet leaves on and the result
// record the owner's result plan places. The record is variable on the
// resource wire; the owner stores it zero-padded to its schema's widest
// field, so a byte no field covers reads zero rather than stale data.
struct RuleResult {
  gate_idx_t gate = DROP_GATE;
  std::vector<std::byte> bytes;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RULE_RESULT_H_
