// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/runtime_state.h"

#include <cctype>
#include <cerrno>
#include <sstream>

#include "module.h"
#include "port.h"
#include "runtime/worker_manager.h"
#include "dataplane/transaction_engine.h"
#include "rcu/rcu_domain.h"
#include "traffic_class.h"
#include "utils/common.h"

namespace bess {
namespace runtime {

namespace {

// Both registries and both legacy helpers (PortBuilder::GenerateDefaultPortName,
// ModuleGraph::GenerateDefaultName) derive a name template from the type name
// when the type has no explicit template.
std::string NameTemplate(const std::string &class_name,
                         const std::string &default_template) {
  if (default_template != "") {
    return default_template;
  }

  std::ostringstream ss;
  char last_char = '\0';
  for (auto t : class_name) {
    if (last_char != '\0' && islower(last_char) && isupper(t)) {
      ss << '_';
    }

    ss << char(tolower(t));
    last_char = t;
  }
  return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// PortRegistry
// ---------------------------------------------------------------------------

Port *PortRegistry::Find(const std::string &name) const {
  auto it = ports_.find(name);
  return it == ports_.end() ? nullptr : it->second.get();
}

bool PortRegistry::Add(std::unique_ptr<Port> &&port) {
  if (ports_.count(port->name())) {
    return false;
  }
  ports_.emplace(port->name(), std::move(port));
  return true;
}

std::unique_ptr<Port> PortRegistry::Remove(const std::string &name) {
  auto it = ports_.find(name);
  if (it == ports_.end()) {
    return nullptr;
  }
  std::unique_ptr<Port> port = std::move(it->second);
  ports_.erase(it);
  return port;
}

int PortRegistry::Destroy(const std::string &name) {
  auto it = ports_.find(name);
  if (it == ports_.end()) {
    return -ENOENT;
  }

  Port *p = it->second.get();
  for (packet_dir_t dir : {PACKET_DIR_INC, PACKET_DIR_OUT}) {
    for (queue_t i = 0; i < p->num_queues[dir]; i++) {
      if (p->users[dir][i]) {
        return -EBUSY;
      }
    }
  }

  std::unique_ptr<Port> owned = std::move(it->second);
  ports_.erase(it);
  owned->DeInit();
  return 0;
}

std::string PortRegistry::GenerateDefaultName(
    const std::string &driver_name,
    const std::string &default_template) const {
  const std::string name_template =
      NameTemplate(driver_name, default_template);

  for (int i = 0;; i++) {
    std::ostringstream ss;
    ss << name_template << i;
    std::string name = ss.str();

    if (!ports_.count(name)) {
      return name;  // found an unallocated name!
    }
  }

  promise_unreachable();
}

void PortRegistry::Clear() {
  for (auto &pair : ports_) {
    pair.second->DeInit();
  }
  ports_.clear();
}

// ---------------------------------------------------------------------------
// ModuleRegistry
// ---------------------------------------------------------------------------

Module *ModuleRegistry::Find(const std::string &name) const {
  auto it = modules_.find(name);
  return it == modules_.end() ? nullptr : it->second.get();
}

bool ModuleRegistry::Add(std::unique_ptr<Module> &&module) {
  if (modules_.count(module->name())) {
    return false;
  }
  modules_.emplace(module->name(), std::move(module));
  return true;
}

std::unique_ptr<Module> ModuleRegistry::Remove(const std::string &name) {
  auto it = modules_.find(name);
  if (it == modules_.end()) {
    return nullptr;
  }
  std::unique_ptr<Module> module = std::move(it->second);
  modules_.erase(it);
  return module;
}

std::string ModuleRegistry::GenerateDefaultName(
    const std::string &class_name, const std::string &default_template) const {
  const std::string name_template = NameTemplate(class_name, default_template);

  for (int i = 0;; i++) {
    std::ostringstream ss;
    ss << name_template << i;
    std::string name = ss.str();

    if (!modules_.count(name)) {
      return name;
    }
  }

  promise_unreachable();
}

// ---------------------------------------------------------------------------
// TrafficClassRegistry
// ---------------------------------------------------------------------------

bool TrafficClassRegistry::Register(std::unique_ptr<TrafficClass> &&c) {
  if (classes_.count(c->name())) {
    return false;
  }
  classes_.emplace(c->name(), std::move(c));
  return true;
}

TrafficClass *TrafficClassRegistry::Find(const std::string &name) const {
  auto it = classes_.find(name);
  return it == classes_.end() ? nullptr : it->second.get();
}

bool TrafficClassRegistry::Release(TrafficClass *c) {
  auto it = classes_.find(c->name());
  if (it == classes_.end() || it->second.get() != c) {
    return false;
  }
  // Forget the object without destroying it: the caller owns the teardown.
  it->second.release();
  classes_.erase(it);
  return true;
}

void TrafficClassRegistry::ReleaseTree(TrafficClass *root) {
  for (TrafficClass *child : root->Children()) {
    ReleaseTree(child);
  }
  Release(root);
}

void TrafficClassRegistry::ReleaseAll() {
  for (auto &pair : classes_) {
    pair.second.release();
  }
  classes_.clear();
}

// ---------------------------------------------------------------------------
// RuntimeState
// ---------------------------------------------------------------------------

RuntimeState::RuntimeState()
    : workers_(std::make_unique<WorkerManager>()),
      // One domain for the whole runtime, sized by the worker id space: a
      // worker registers when its thread starts and unregisters when it is
      // done, so a recreated worker reuses its id.
      rcu_(std::make_unique<rcu::RcuDomain>(Worker::kMaxWorkers)),
      transactions_(std::make_unique<dataplane::TransactionEngine>(*rcu_)) {
  builtin_metrics_ = metrics_.Register([this](stats::MetricWriter &w) {
    const rcu::RcuStats r = rcu_->Stats();
    w.Counter("bess_rcu_grace_periods_started_total", "Grace periods started", r.grace_periods_started);
    w.Counter("bess_rcu_grace_periods_completed_total", "Grace periods completed",
              r.grace_periods_completed);
    w.Counter("bess_rcu_objects_retired_total", "Objects retired to RCU", r.objects_retired);
    w.Counter("bess_rcu_objects_reclaimed_total", "Retired objects destroyed", r.objects_reclaimed);
    w.Gauge("bess_rcu_pending_retired_objects",
            "Retired objects waiting for a grace period (reclamation backlog)",
            static_cast<double>(r.pending_retired_objects));
    w.Gauge("bess_rcu_online_readers", "Readers (workers) online", rcu_->online_readers());
    // In Outcome's order (checked below).
    static constexpr const char *kOutcomes[] = {"applied", "rejected", "conflict", "busy",
                                                "unsupported"};
    static_assert(static_cast<size_t>(dataplane::TransactionEngine::Outcome::kUnsupported) == 4 &&
                  static_cast<size_t>(dataplane::TransactionEngine::Outcome::kApplied) == 0);
    const auto counts = transactions_->outcome_counts();
    for (size_t i = 0; i < std::size(kOutcomes); i++) {
      w.Counter("bess_transactions_total", "Dataplane transactions by outcome",
                static_cast<double>(counts[i]), {{"outcome", kOutcomes[i]}});
    }
    w.Gauge("bess_transaction_generation", "Dataplane transaction generation",
            static_cast<double>(transactions_->generation()));
    w.Gauge("bess_transaction_pending_cascades",
            "Removal cascades waiting for a grace period (Apply answers BUSY past 4096)",
            static_cast<double>(transactions_->pending_cascades()));
  });
}

RuntimeState::~RuntimeState() = default;

WorkerManager &RuntimeState::workers() {
  return *workers_;
}

const WorkerManager &RuntimeState::workers() const {
  return *workers_;
}

rcu::RcuDomain &RuntimeState::rcu() {
  return *rcu_;
}

const rcu::RcuDomain &RuntimeState::rcu() const {
  return *rcu_;
}

dataplane::TransactionEngine &RuntimeState::transactions() {
  return *transactions_;
}

RuntimeState &RuntimeState::Get() {
  static RuntimeState state;
  return state;
}

}  // namespace runtime
}  // namespace bess
