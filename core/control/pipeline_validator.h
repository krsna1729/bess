// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_VALIDATOR_H_
#define BESS_CONTROL_PIPELINE_VALIDATOR_H_

#include "control/control_error.h"
#include "control/pipeline_spec.h"
#include "runtime/runtime_state.h"

namespace bess {
namespace control {

// A spec that has passed validation, in canonical form.
struct ValidatedPipeline {
  PipelineSpec spec;
};

// Validates a desired pipeline without touching the runtime (MODERNIZATION.md
// section 9.5): no PMD is created, no module is instantiated, no worker is
// attached, no gate is connected, no queue is acquired, no metadata is
// allocated and no scheduler tree is modified. Everything that can be decided
// structurally is decided here; checks that genuinely require invoking a
// driver or instantiating a module belong to Prepare().
//
// `runtime` is read for what already exists (registries and type registries);
// `desired` is what the caller wants to exist.
ControlResult<ValidatedPipeline> ValidatePipeline(
    const runtime::RuntimeState &runtime, const PipelineSpec &desired);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_VALIDATOR_H_
