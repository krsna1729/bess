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

#include "control/pipeline_spec.h"

namespace bess {
namespace control {

namespace {

bool SameAny(const google::protobuf::Any &a, const google::protobuf::Any &b) {
  return a.SerializeAsString() == b.SerializeAsString();
}

}  // namespace

bool PortSpec::operator==(const PortSpec &other) const {
  return name == other.name && driver == other.driver &&
         num_rx_queues == other.num_rx_queues &&
         num_tx_queues == other.num_tx_queues &&
         rx_queue_size == other.rx_queue_size &&
         tx_queue_size == other.tx_queue_size && SameAny(arg, other.arg);
}

bool ModuleSpec::operator==(const ModuleSpec &other) const {
  return name == other.name && mclass == other.mclass &&
         SameAny(arg, other.arg);
}

void Normalize(PipelineSpec *spec) {
  for (PortSpec &port : spec->ports) {
    if (port.num_rx_queues == 0) {
      port.num_rx_queues = 1;
    }
    if (port.num_tx_queues == 0) {
      port.num_tx_queues = 1;
    }
  }

  std::sort(spec->ports.begin(), spec->ports.end(),
            [](const PortSpec &a, const PortSpec &b) { return a.name < b.name; });

  std::sort(spec->modules.begin(), spec->modules.end(),
            [](const ModuleSpec &a, const ModuleSpec &b) {
              return a.name < b.name;
            });

  std::sort(spec->connections.begin(), spec->connections.end(),
            [](const ConnectionSpec &a, const ConnectionSpec &b) {
              if (a.upstream != b.upstream) {
                return a.upstream < b.upstream;
              }
              return a.ogate < b.ogate;
            });

  std::sort(spec->workers.begin(), spec->workers.end(),
            [](const WorkerSpec &a, const WorkerSpec &b) {
              return a.wid < b.wid;
            });

  std::sort(spec->traffic_classes.begin(), spec->traffic_classes.end(),
            [](const TrafficClassSpec &a, const TrafficClassSpec &b) {
              return a.name < b.name;
            });
}

}  // namespace control
}  // namespace bess
