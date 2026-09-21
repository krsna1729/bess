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

#ifndef BESS_CONTROL_PIPELINE_SPEC_H_
#define BESS_CONTROL_PIPELINE_SPEC_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gate.h"
#include "message.h"
#include "port.h"

namespace bess {
namespace control {

// Desired-state representation of a pipeline (MODERNIZATION.md section 9.3).
// This is the in-process domain model: protobuf is a wire format that the RPC
// adapter converts to and from these types, never the thing the control plane
// reasons about.
//
// Desired state requires explicit stable names -- unlike the legacy imperative
// RPCs, which may generate one -- because diff, reapply, idempotency,
// diagnostics and generation comparisons all depend on desired-state identity.

struct PortSpec {
  std::string name;
  std::string driver;
  queue_t num_rx_queues = 0;  // 0 means "one queue" / "driver default"
  queue_t num_tx_queues = 0;
  uint64_t rx_queue_size = 0;  // 0 means "driver default"
  uint64_t tx_queue_size = 0;
  google::protobuf::Any arg;

  // Protobuf messages have no value equality, so arguments are compared by
  // their serialized bytes.
  bool operator==(const PortSpec &other) const;
};

struct ModuleSpec {
  std::string name;
  std::string mclass;
  google::protobuf::Any arg;

  bool operator==(const ModuleSpec &other) const;
};

struct ConnectionSpec {
  std::string upstream;
  gate_idx_t ogate = 0;
  std::string downstream;
  gate_idx_t igate = 0;
  bool skip_default_hooks = false;

  bool operator==(const ConnectionSpec &other) const = default;
};

// One end of a disconnect: the upstream module and the output gate to clear.
struct DisconnectionSpec {
  std::string name;
  gate_idx_t ogate = 0;

  bool operator==(const DisconnectionSpec &other) const = default;
};

struct WorkerSpec {
  int wid = -1;
  int core = -1;
  std::string scheduler;

  bool operator==(const WorkerSpec &other) const = default;
};

struct TrafficClassSpec {
  std::string name;
  std::string parent;
  std::string policy;
  std::string resource;
  int wid = -1;
  bool has_priority = false;
  int64_t priority = 0;
  bool has_share = false;
  int64_t share = 0;
  std::map<std::string, int64_t> limit;
  std::map<std::string, int64_t> max_burst;
  std::string leaf_module_name;
  uint64_t leaf_module_taskid = 0;

  bool operator==(const TrafficClassSpec &other) const = default;
};

struct PipelineSpec {
  std::vector<PortSpec> ports;
  std::vector<ModuleSpec> modules;
  std::vector<ConnectionSpec> connections;
  std::vector<WorkerSpec> workers;
  std::vector<TrafficClassSpec> traffic_classes;

  bool operator==(const PipelineSpec &other) const = default;
};

// Puts a spec into canonical form: stable ordering and normalized defaults, so
// that two descriptions of the same pipeline compare equal and diffs do not
// churn (section 9.6). Sorting is by name (and by (upstream, ogate) for
// connections, by wid for workers); queue counts default to one queue, since
// that is what the runtime resolves 0 to.
void Normalize(PipelineSpec *spec);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_PIPELINE_SPEC_H_
