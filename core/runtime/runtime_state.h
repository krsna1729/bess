// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_RUNTIME_STATE_H_
#define BESS_RUNTIME_RUNTIME_STATE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "framework/instance_registry.h"
#include "stats/metric_registry.h"
#include "framework/module_requests.h"

class Module;
class Port;

namespace bess {
namespace rcu {
class RcuDomain;
}  // namespace rcu
namespace dataplane {
class TransactionEngine;
}  // namespace dataplane
}  // namespace bess

namespace bess {
class TrafficClass;

namespace runtime {

class WorkerManager;

// Mutable instance state has an owner.
//
// The builders (`ModuleBuilder`, `PortBuilder`, `GateHookBuilder`,
// `ResumeHookBuilder`) stay process-global: they describe available *types* and
// are registration metadata. Instance state -- the modules and ports that
// actually exist in the running pipeline -- is owned here, as `unique_ptr`s, so
// that staging, snapshotting, testing and (Phase G0's goal) transactions have
// something explicit to operate on.
//
// Control-plane code is the only writer and is serialized by the control-plane
// lock; the dataplane keeps using the non-owning pointers it already holds
// (lifetime is bounded by the owning registry, not by the dataplane).

// Owns the live ports, keyed by name.
class PortRegistry {
 public:
  using Map = std::map<std::string, std::unique_ptr<Port>>;

  Port *Find(const std::string &name) const;
  bool Contains(const std::string &name) const { return Find(name) != nullptr; }
  size_t Size() const { return ports_.size(); }
  bool Empty() const { return ports_.empty(); }

  // Non-owning view, for readers that need to iterate. The map values are
  // `unique_ptr`s: `it->second.get()` is the raw pointer.
  const Map &All() const { return ports_; }

  // Takes ownership of `port` on success. Returns false -- leaving ownership
  // with the caller -- if a port with that name is already registered.
  bool Add(std::unique_ptr<Port> &&port);

  // Erases the entry and hands ownership back to the caller. Returns nullptr if
  // the name is not registered. The caller is responsible for DeInit/destroy.
  std::unique_ptr<Port> Remove(const std::string &name);

  // Refuses to destroy a port whose queues are in use (-EBUSY); otherwise
  // DeInits and destroys it. Returns 0 or -errno, like the legacy helper.
  int Destroy(const std::string &name);

  // Generates an unused name from the driver name and its template.
  std::string GenerateDefaultName(const std::string &driver_name,
                                  const std::string &default_template) const;

  // Destroys every port this registry owns (test/teardown helper).
  void Clear();

 private:
  Map ports_;
};

// Owns the live modules, keyed by name, plus the set of modules that are
// scheduling tasks (what `ModuleGraph::tasks_` used to be).
class ModuleRegistry {
 public:
  using Map = std::map<std::string, std::unique_ptr<Module>>;

  Module *Find(const std::string &name) const;
  bool Contains(const std::string &name) const { return Find(name) != nullptr; }
  size_t Size() const { return modules_.size(); }
  bool Empty() const { return modules_.empty(); }

  // Non-owning view, for readers that need to iterate.
  const Map &All() const { return modules_; }

  // Takes ownership of `module` on success. Returns false -- leaving ownership
  // with the caller -- if a module with that name is already registered.
  bool Add(std::unique_ptr<Module> &&module);

  // Erases the entry and hands ownership back to the caller (nullptr if the
  // name is not registered). The caller runs `Module::Destroy()` and lets the
  // returned pointer die.
  std::unique_ptr<Module> Remove(const std::string &name);

  // Task membership: modules that own at least one scheduling task.
  bool MarkTask(const std::string &name) { return task_names_.insert(name).second; }
  void UnmarkTask(const std::string &name) { task_names_.erase(name); }
  const std::unordered_set<std::string> &TaskNames() const {
    return task_names_;
  }

  // Generates an unused module name from the class name and its template.
  std::string GenerateDefaultName(const std::string &class_name,
                                  const std::string &default_template) const;

  // Destroys every module this registry owns (test/teardown helper).
  void Clear();

 private:
  Map modules_;
  std::unordered_set<std::string> task_names_;
};

// Owns the live traffic classes, keyed by name. Traffic classes form trees
// (scheduler roots, TC hierarchies, leaf classes attached to module tasks), so
// teardown paths that delete a tree hand the entries back before deleting:
// `Release`/`ReleaseTree` erase registry entries without destroying objects,
// which is what keeps destruction out of the destructors.
class TrafficClassRegistry {
 public:
  using Map = std::map<std::string, std::unique_ptr<TrafficClass>>;

  // Takes ownership on success; false (leaving ownership with the caller) if
  // the name is already registered.
  bool Register(std::unique_ptr<TrafficClass> &&c);

  TrafficClass *Find(const std::string &name) const;
  bool Contains(const std::string &name) const { return Find(name) != nullptr; }
  size_t Size() const { return classes_.size(); }
  bool Empty() const { return classes_.empty(); }

  // Non-owning view, for readers that need to iterate.
  const Map &All() const { return classes_; }

  // Erases `c`'s entry (and, for ReleaseTree, its descendants' entries)
  // without deleting anything. The caller destroys the objects.
  bool Release(TrafficClass *c);
  void ReleaseTree(TrafficClass *root);

  // Forgets every traffic class without destroying it (legacy `ClearAll`
  // semantics: the schedulers and modules that own those trees still delete
  // them, and unowned orphans are leaked exactly as before).
  void ReleaseAll();

 private:
  Map classes_;
};

// The state of the running pipeline. G0 keeps exactly one active instance
// (created on first use); later commits hand it to the transaction engine
// explicitly instead of reaching for it.
class RuntimeState {
 public:
  static RuntimeState &Get();

  PortRegistry &ports() { return ports_; }
  ModuleRegistry &modules() { return modules_; }
  TrafficClassRegistry &traffic_classes() { return traffic_classes_; }
  // Application-owned object graphs shared by modules (D-045).
  framework::InstanceRegistry &instances() { return instances_; }
  // Module requests from workers, delivered by the control side's maintenance
  // loop (TP4, D-077).
  framework::RequestHub &requests() { return requests_; }
  WorkerManager &workers();

  // The single dataplane reader domain (K1): workers register once and report
  // quiescence, and every immutable dataplane object publishes and retires
  // through it. Its lifetime is the runtime's, so it must outlive every
  // registered reader.
  rcu::RcuDomain &rcu();

  // The runtime's transaction engine (G1.2b, D-021): modules register the
  // tables they want changed atomically with other modules' tables as
  // resources (D-022). Callers of Apply() hold the control-plane lock, as
  // module commands do: a module's own commands write the same tables.
  dataplane::TransactionEngine &transactions();

  // Operational metrics (M25): the runtime's own sources (RCU, transaction
  // engine) are registered here; modules register theirs through
  // ModuleInitContext::metrics(). Read by the ListMetrics RPC.
  stats::MetricRegistry &metrics() { return metrics_; }

  const PortRegistry &ports() const { return ports_; }
  const ModuleRegistry &modules() const { return modules_; }
  const TrafficClassRegistry &traffic_classes() const {
    return traffic_classes_;
  }
  const WorkerManager &workers() const;
  const rcu::RcuDomain &rcu() const;

  // Monotonic control-plane generation: bumped exactly once per successful
  // state-changing transaction, never for reads, validation, planning, failed
  // transactions or a no-op apply.
  uint64_t generation() const { return generation_; }
  void BumpGeneration() { generation_++; }

  RuntimeState(const RuntimeState &) = delete;
  RuntimeState &operator=(const RuntimeState &) = delete;

 private:
  RuntimeState();
  ~RuntimeState();

  // Before everything that registers a source in it.
  stats::MetricRegistry metrics_;
  // First: modules (below) release their leases on it as they are destroyed.
  framework::InstanceRegistry instances_;
  // Before modules_ for the same reason: a module's endpoints unregister as it
  // is destroyed.
  framework::RequestHub requests_;
  PortRegistry ports_;
  ModuleRegistry modules_;
  TrafficClassRegistry traffic_classes_;
  std::unique_ptr<WorkerManager> workers_;
  std::unique_ptr<rcu::RcuDomain> rcu_;
  // After rcu_: it retires through it, so it goes first.
  std::unique_ptr<dataplane::TransactionEngine> transactions_;
  uint64_t generation_ = 0;
  // Last: removed before what it reads.
  stats::MetricSource builtin_metrics_;
};

// Accessor for code that needs the runtime it is operating in. Module
// initialization uses this to resolve the ports it references instead of
// reaching into a global registry (MODERNIZATION.md section 9.2).
inline RuntimeState &runtime() {
  return RuntimeState::Get();
}

}  // namespace runtime
}  // namespace bess

#endif  // BESS_RUNTIME_RUNTIME_STATE_H_
