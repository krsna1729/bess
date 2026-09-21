// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CONTROL_PIPELINE_VALIDATOR_H_
#define BESS_CONTROL_PIPELINE_VALIDATOR_H_

#include "control/control_error.h"
#include "control/pipeline_spec.h"
#include "control/runtime_state.h"

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
ControlResult<ValidatedPipeline> ValidatePipeline(const RuntimeState &runtime,
                                                  const PipelineSpec &desired);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_VALIDATOR_H_
