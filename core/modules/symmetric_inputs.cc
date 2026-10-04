// SPDX-License-Identifier: BSD-3-Clause

#include "modules/symmetric_inputs.h"

#include <map>
#include <set>

#include "gate.h"
#include "module.h"
#include "modules/port_inc.h"
#include "modules/queue_inc.h"
#include "scheduler.h"
#include "task.h"
#include "traffic_class.h"
#include "utils/format.h"

namespace bess::modules {

namespace {

// The root of the scheduler tree `t` runs under: one per worker, so two
// tasks with the same root run on the same worker. nullptr when unattached.
const bess::TrafficClass *RootOf(const Task &t) {
  const bess::LeafTrafficClass *leaf = t.tc();
  return leaf != nullptr ? leaf->Root() : nullptr;
}

}  // namespace

std::optional<std::string> AsymmetricInputs(const Module &m) {
  if (m.visited_tasks().empty()) {
    return std::string("no task reaches it");
  }
  // The module must receive straight from the port queues: a module in
  // between may rewrite or decapsulate, and the NIC hashed the tuple it saw
  // (a NAT in front, or a tunnel's outer header), not the one tracked here.
  std::set<const Module *> direct;
  for (const bess::IGate *igate : m.igates()) {
    if (igate == nullptr) {
      continue;
    }
    for (const bess::OGate *og : igate->ogates_upstream()) {
      direct.insert(og->module());
    }
  }
  uint64_t queues = 0;
  uint64_t signature = 0;
  std::map<uint64_t, const bess::TrafficClass *> root_of_queue;
  for (const Task *t : m.visited_tasks()) {
    if (direct.count(t->module()) == 0) {
      return bess::utils::Format("its input from '%s' passes through other modules",
                                 t->module()->name().c_str());
    }
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
      signature = port->rss_signature();
    } else if (port->num_rx_queues() != queues) {
      return bess::utils::Format("port '%s' has %llu receive queues, another %llu",
                                 port->name().c_str(),
                                 static_cast<unsigned long long>(port->num_rx_queues()),
                                 static_cast<unsigned long long>(queues));
    } else if (queues > 1 && port->rss_signature() != signature) {
      // Several queues: the same tuple must pick the same queue index on
      // every port (driver, hash, key, fields, redirection table).
      return bess::utils::Format("port '%s' spreads packets over its queues differently",
                                 port->name().c_str());
    }
    const auto [it, fresh] = root_of_queue.emplace(qid, RootOf(*t));
    if (!fresh && it->second != RootOf(*t)) {
      return bess::utils::Format("queue %llu of two ports runs on two workers",
                                 static_cast<unsigned long long>(qid));
    }
  }
  return std::nullopt;
}

}  // namespace bess::modules
