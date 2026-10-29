// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_MODULE_INIT_CONTEXT_H_
#define BESS_FRAMEWORK_MODULE_INIT_CONTEXT_H_

#include <string>

// Decision D-042 (docs/decisions.md): a module obtains the runtime facilities
// it needs through explicit, narrow capabilities, once, during construction or
// Init(). Packet processing never performs capability lookup, and a module
// never includes runtime/runtime_state.h.

class Module;
class Port;

namespace bess {
namespace rcu {
class RcuDomain;
}  // namespace rcu
namespace stats {
class EventHub;
class MetricRegistry;
}  // namespace stats
namespace dataplane {
class ResourceRegistry;
}  // namespace dataplane
namespace framework {
class RequestHub;
class TickHub;
}  // namespace framework
namespace framework {
class InstanceRegistry;
class MemoryScope;
class ResourceBindings;
}  // namespace framework
namespace runtime {
class PortRegistry;
}  // namespace runtime

namespace framework {

// Read-only view of the ports that exist, by name.
class PortDirectory {
 public:
  explicit PortDirectory(const runtime::PortRegistry &registry) noexcept
      : registry_(registry) {}

  // nullptr if no port has that name.
  Port *Find(const std::string &name) const;

 private:
  const runtime::PortRegistry &registry_;
};

// What a module may use while it is being created and initialized. The
// referenced objects outlive every module created against the context.
//
// Each capability is a named method rather than a service lookup, so a
// module's dependencies are visible in review and can be narrowed later
// without touching callers.
class ModuleInitContext {
 public:
  ModuleInitContext(dataplane::ResourceRegistry &resources,
                    ResourceBindings &resource_bindings,
                    framework::InstanceRegistry &instances, rcu::RcuDomain &rcu,
                    const runtime::PortRegistry &ports, stats::MetricRegistry &metrics,
                    framework::RequestHub &requests, stats::EventHub &events,
                    framework::TickHub &ticks) noexcept
      : resources_(resources),
        resource_bindings_(resource_bindings),
        instances_(instances),
        rcu_(rcu),
        ports_(ports),
        metrics_(metrics),
        requests_(requests),
        events_(events),
        ticks_(ticks) {}

  ModuleInitContext(const ModuleInitContext &) = delete;
  ModuleInitContext &operator=(const ModuleInitContext &) = delete;

  // Registers and releases this instance's resources (D-021, D-022): the
  // engine's public face. Callers hold the control-plane lock, as module
  // commands do.
  dataplane::ResourceRegistry &resources() const noexcept { return resources_; }

  // The reader domain every published table retires through. Needed by
  // modules that own an RcuPtr or a classifier/route table.
  rcu::RcuDomain &rcu() const noexcept { return rcu_; }

  const PortDirectory &ports() const noexcept { return ports_; }

  // Application-owned objects modules share by name (D-045). Resolve once, in
  // Init(), keep the lease, and cache the pointer; never from the packet path.
  framework::InstanceRegistry &instances() const noexcept { return instances_; }

  // Operational metrics (M25, stats/metric_registry.h): register a source that
  // reads this module's counters when the control plane asks; keep the
  // returned MetricSource as a member, declared after what it reads. A plugin
  // that calls this requires BESS_CAP_METRICS.
  stats::MetricRegistry &metrics() const noexcept { return metrics_; }
  // Worker-to-control requests (TP4, framework/module_requests.h): open a
  // RequestEndpoint on it in Init(); workers post, the daemon's maintenance
  // loop calls the handler under the control-plane lock. A plugin that calls
  // this requires BESS_CAP_REQUESTS.
  framework::RequestHub &requests() const noexcept { return requests_; }
  // Operational events (M25 phase 2, stats/event_hub.h): register a type in
  // Init(); a worker posts with events().Post(ctx->wid, ...), never blocking;
  // WatchEvents streams them. A plugin that calls this requires
  // BESS_CAP_EVENTS.
  stats::EventHub &events() const noexcept { return events_; }
  // Periodic control-side work (D-150, framework/module_tick.h): open a
  // TickEndpoint on it in Init(); the daemon's maintenance loop calls the
  // callback under the control-plane lock with a time slice, and appends the
  // events it emits after the lock is released. A plugin that calls this
  // requires BESS_CAP_TICKS.
  framework::TickHub &ticks() const noexcept { return ticks_; }
  // Wire codecs for this module's resources (D-044, D-094; experimental:
  // framework/resource_bindings.h and framework/resource_codec.h). Bind a
  // codec to each resource registered through resources() so the control
  // API (ApplyTransaction, ListTransactionResources) can reach it; keep the
  // returned ResourceBinding as a member declared after the resource. Codecs
  // are protobuf-bound control metadata: they never reach the dataplane
  // libraries. A plugin that calls this requires BESS_CAP_RESOURCES (the
  // bindings have been in every bessd that checks a plugin descriptor).
  ResourceBindings &codecs() const noexcept { return resource_bindings_; }
  // This module's memory (D-198, experimental: framework/memory_scope.h):
  // every table, pool, ring or other primitive the module sizes from its
  // arguments is admitted here before it is allocated -- Create<T>(resource,
  // args...), or Admit/Lease and a primitive built on socket() -- so one
  // that does not fit is refused whole (RESOURCE_EXHAUSTED) and what each
  // module holds is reported (GetResourceStatus.memory.modules). Unlike the
  // other capabilities it is the module's own, destroyed with it, which
  // releases whatever it still holds. A plugin that calls it requires
  // BESS_CAP_MEMORY (daemons before D-198 lack the scope's symbols).
  MemoryScope &memory() const noexcept { return *memory_; }

  // The framework gives every module its context; a module reads it through
  // Module::init_context() and cannot construct or look one up itself.
 private:
  // The context for the process's one active runtime. A separate context per
  // runtime is future work; Module's constructor is the only caller, so
  // ProcessDefault() is not part of the author-facing surface. Each module
  // gets a copy of it bound to the module's own memory scope.
  friend class ::Module;
  static const ModuleInitContext &ProcessDefault();
  ModuleInitContext(const ModuleInitContext &shared, MemoryScope &memory) noexcept
      : resources_(shared.resources_),
        resource_bindings_(shared.resource_bindings_),
        instances_(shared.instances_),
        rcu_(shared.rcu_),
        ports_(shared.ports_),
        metrics_(shared.metrics_),
        requests_(shared.requests_),
        events_(shared.events_),
        ticks_(shared.ticks_),
        memory_(&memory) {}

  dataplane::ResourceRegistry &resources_;
  ResourceBindings &resource_bindings_;
  framework::InstanceRegistry &instances_;
  rcu::RcuDomain &rcu_;
  PortDirectory ports_;
  // Appended in order (M25 metrics, TP4, M25 events): plugins built before
  // each read the members above it at unchanged offsets.
  stats::MetricRegistry &metrics_;
  framework::RequestHub &requests_;
  stats::EventHub &events_;
  framework::TickHub &ticks_;
  MemoryScope *memory_ = nullptr;  // nullptr in ProcessDefault() only
};

}  // namespace framework
}  // namespace bess

#endif  // BESS_FRAMEWORK_MODULE_INIT_CONTEXT_H_
