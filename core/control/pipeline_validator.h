// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_VALIDATOR_H_
#define BESS_CONTROL_PIPELINE_VALIDATOR_H_

#include <map>
#include <string>

#include "commands.h"
#include "control/control_error.h"
#include "control/pipeline_spec.h"
#include "runtime/runtime_state.h"

namespace bess {
namespace control {

// What a module would promise (D-180): its class's answer for its init
// argument, and the class's resource tables, which say how it changes.
struct ModuleCapabilityReport {
  ModuleCapabilities declared;
  ResourceTables tables;
};

// A spec that has passed validation, in canonical form.
struct ValidatedPipeline {
  PipelineSpec spec;
  std::map<std::string, ModuleCapabilityReport> capabilities;  // by module name
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
