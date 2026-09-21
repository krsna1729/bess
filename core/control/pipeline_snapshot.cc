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

#include "control/pipeline_snapshot.h"

#include <algorithm>
#include <vector>

#include "control/worker_manager.h"
#include "module.h"
#include "port.h"
#include "task.h"
#include "traffic_class.h"
#include "worker.h"

namespace bess {
namespace control {

namespace {

bool SameAny(const google::protobuf::Any &a, const google::protobuf::Any &b) {
  return a.SerializeAsString() == b.SerializeAsString();
}

}  // namespace

bool PortSnapshot::operator==(const PortSnapshot &other) const {
  return name == other.name && driver == other.driver &&
         mac_addr == other.mac_addr &&
         num_rx_queues == other.num_rx_queues &&
         num_tx_queues == other.num_tx_queues &&
         rx_queue_size == other.rx_queue_size &&
         tx_queue_size == other.tx_queue_size &&
         SameAny(driver_arg, other.driver_arg);
}

bool ModuleSnapshot::operator==(const ModuleSnapshot &other) const {
  return name == other.name && mclass == other.mclass &&
         SameAny(arg, other.arg);
}

PipelineSnapshot SnapshotRuntime(const RuntimeState &runtime) {
  PipelineSnapshot snapshot;

  for (const auto &pair : runtime.ports().All()) {
    const Port *port = pair.second.get();
    PortSnapshot entry;
    entry.name = port->name();
    entry.driver = port->port_builder()->class_name();
    entry.mac_addr = port->conf().mac_addr.ToString();
    entry.num_rx_queues = port->num_queues[PACKET_DIR_INC];
    entry.num_tx_queues = port->num_queues[PACKET_DIR_OUT];
    entry.rx_queue_size = port->queue_size[PACKET_DIR_INC];
    entry.tx_queue_size = port->queue_size[PACKET_DIR_OUT];
    entry.driver_arg = port->driver_arg();
    snapshot.ports.push_back(std::move(entry));
  }

  for (const auto &pair : runtime.modules().All()) {
    const Module *module = pair.second.get();
    ModuleSnapshot entry;
    entry.name = module->name();
    entry.mclass = module->module_builder()->class_name();
    entry.arg = module->initial_arg();
    snapshot.modules.push_back(std::move(entry));
  }

  // Connections are derived from the graph itself: for every active output
  // gate, record where it points. Modules iterate in name order and gates in
  // index order, so the result is stable.
  for (const auto &pair : runtime.modules().All()) {
    const Module *module = pair.second.get();
    const auto &ogates = module->ogates();
    for (gate_idx_t i = 0; i < ogates.size(); i++) {
      if (!is_active_gate(ogates, i)) {
        continue;
      }
      const bess::IGate *igate = ogates[i]->igate();
      if (!igate) {
        continue;
      }
      ConnectionSnapshot entry;
      entry.upstream = module->name();
      entry.ogate = i;
      entry.downstream = igate->module()->name();
      entry.igate = igate->gate_idx();
      snapshot.connections.push_back(std::move(entry));
    }
  }

  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    const Worker *worker = runtime.workers().Get(wid);
    if (!worker) {
      continue;
    }
    WorkerSnapshot entry;
    entry.wid = wid;
    entry.core = worker->core();
    entry.scheduler = runtime.workers().scheduler_name(wid);
    snapshot.workers.push_back(std::move(entry));
  }

  for (const auto &pair : runtime.traffic_classes().All()) {
    const TrafficClass *c = pair.second.get();
    TrafficClassSnapshot entry;
    entry.name = c->name();
    entry.parent = c->parent() ? c->parent()->name() : "";
    entry.policy = (c->policy() >= 0 && c->policy() < NUM_POLICIES)
                       ? TrafficPolicyName[c->policy()]
                       : "invalid";
    entry.wid = c->WorkerId();

    if (c->policy() == POLICY_LEAF) {
      const auto *leaf = static_cast<const LeafTrafficClass *>(c);
      const Task *task = leaf->task();
      const Module *module = task->module();
      entry.leaf_module_name = module->name();

      const auto &tasks = module->tasks();
      auto it = std::find(tasks.begin(), tasks.end(), task);
      if (it != tasks.end()) {
        entry.leaf_module_taskid = it - tasks.begin();
      }
    }

    snapshot.traffic_classes.push_back(std::move(entry));
  }

  return snapshot;
}

}  // namespace control
}  // namespace bess
