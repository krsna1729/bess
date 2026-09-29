// SPDX-License-Identifier: BSD-3-Clause

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
