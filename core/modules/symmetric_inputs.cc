// SPDX-License-Identifier: BSD-3-Clause

#include "modules/symmetric_inputs.h"

#include <map>

#include "module.h"
#include "modules/port_inc.h"
#include "modules/queue_inc.h"
#include "runtime/runtime_state.h"
#include "runtime/worker_manager.h"
#include "scheduler.h"
#include "task.h"
#include "traffic_class.h"
#include "utils/format.h"
#include "worker.h"

namespace bess::modules {

namespace {

// The worker whose scheduler runs `t`, or -1.
int WorkerOf(const Task &t) {
  const bess::LeafTrafficClass *leaf = t.tc();
  if (leaf == nullptr) {
    return -1;
  }
  const bess::TrafficClass *root = leaf->Root();
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    const Worker *w = runtime::runtime().workers().Get(wid);
    if (w != nullptr && w->scheduler()->root() == root) {
      return wid;
    }
  }
  return -1;
}

}  // namespace

std::optional<std::string> AsymmetricInputs(const Module &m) {
  if (m.visited_tasks().empty()) {
    return std::string("no task reaches it");
  }
  uint64_t queues = 0;
  std::map<uint64_t, int> worker_of_queue;
  for (const Task *t : m.visited_tasks()) {
    const Port *port = nullptr;
    uint64_t qid = 0;
    if (const auto *in = dynamic_cast<const PortInc *>(t->module())) {
      port = in->port();
      qid = reinterpret_cast<uintptr_t>(t->arg());
    } else if (const auto *q = dynamic_cast<const QueueInc *>(t->module())) {
      port = q->port();
      qid = q->qid();
    } else {
      return bess::utils::Format("its input '%s' is not a port queue",
                                 t->module()->name().c_str());
    }
    if (port == nullptr || !port->symmetric_rss()) {
      return bess::utils::Format(
          "port '%s' has no symmetric hash (PMDPortArg.symmetric_rss)",
          port != nullptr ? port->name().c_str() : "?");
    }
    if (queues == 0) {
      queues = port->num_rx_queues();
    } else if (port->num_rx_queues() != queues) {
      return bess::utils::Format("port '%s' has %llu receive queues, another %llu",
                                 port->name().c_str(),
                                 static_cast<unsigned long long>(port->num_rx_queues()),
                                 static_cast<unsigned long long>(queues));
    }
    const int wid = WorkerOf(*t);
    const auto [it, fresh] = worker_of_queue.emplace(qid, wid);
    if (!fresh && it->second != wid) {
      return bess::utils::Format("queue %llu runs on workers %d and %d",
                                 static_cast<unsigned long long>(qid), it->second, wid);
    }
  }
  return std::nullopt;
}

}  // namespace bess::modules
