// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_RESOURCES_H_
#define BESS_CONTROL_PIPELINE_RESOURCES_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "control/control_error.h"
#include "control/pipeline_diff.h"
#include "control/pipeline_spec.h"
#include "dataplane/resource.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"

namespace bess::control {

// Resource contents as desired state (D-129): read back through each
// resource's codec, and converged to by one dataplane transaction -- the
// same decoding and the same engine as ApplyTransaction.
struct ResourceConvergence {
  ResourceDelta delta;
  std::vector<dataplane::Op> ops;  // with build_ops only
  bool requires_paused_workers = false;
};

// The contents of the named resources (all reachable ones if `names` is
// empty), by name; entries in the codec's order. An unknown name is
// NOT_FOUND. Keys and values are in canonical wire form (deterministic
// serialization), so a read-back compares equal to itself.
ControlResult<std::vector<ResourceSpec>> ReadResources(
    const dataplane::TransactionEngine &engine, const framework::ResourceBindings &bindings,
    std::span<const std::string> names);

// Compares `desired` with the resource's contents: every desired key decodes
// (INVALID_ARGUMENT naming the entry if not, or if a key repeats), a key
// absent now is created, a value whose canonical bytes differ is updated,
// and a key desired state does not name is removed. With `build_ops`, also
// decodes the values that change into the transaction's operations (for a
// singleton this stages the value, so a diff does not).
ControlResult<ResourceConvergence> Converge(const dataplane::TransactionEngine &engine,
                                            const framework::ResourceBindings &bindings,
                                            const ResourceSpec &desired, bool build_ops);

}  // namespace bess::control

#endif  // BESS_CONTROL_PIPELINE_RESOURCES_H_
