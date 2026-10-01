// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_MODULE_INIT_CONTEXT_H_
#define BESS_FRAMEWORK_MODULE_INIT_CONTEXT_H_

#include <string>

// Decision D-042 (docs/decisions.md): a module obtains the runtime facilities
// it needs through explicit, narrow capabilities, once, during construction or
// Init(). Packet processing never performs capability lookup, and a module
// never includes runtime/runtime_state.h.

class Port;

namespace bess {
namespace rcu {
class RcuDomain;
}  // namespace rcu
namespace dataplane {
class TransactionEngine;
}  // namespace dataplane
namespace framework {
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
  ModuleInitContext(dataplane::TransactionEngine &resources,
                    ResourceBindings &resource_bindings, rcu::RcuDomain &rcu,
                    const runtime::PortRegistry &ports) noexcept
      : resources_(resources),
        resource_bindings_(resource_bindings),
        rcu_(rcu),
        ports_(ports) {}

  ModuleInitContext(const ModuleInitContext &) = delete;
  ModuleInitContext &operator=(const ModuleInitContext &) = delete;

  // Registers and applies this instance's resources (D-021, D-022). Callers
  // hold the control-plane lock, as module commands do.
  dataplane::TransactionEngine &resources() const noexcept {
    return resources_;
  }

  // Control-side metadata (the wire codec) for resources this module
  // registers, kept apart from the dataplane resource itself (D-044).
  ResourceBindings &resource_bindings() const noexcept {
    return resource_bindings_;
  }

  // The reader domain every published table retires through. Needed by
  // modules that own an RcuPtr or a classifier/route table.
  rcu::RcuDomain &rcu() const noexcept { return rcu_; }

  const PortDirectory &ports() const noexcept { return ports_; }

  // The context for the process's one active runtime instance. Explicit
  // application instances (M5) will bind a module to its own context here.
  static const ModuleInitContext &ProcessDefault();

 private:
  dataplane::TransactionEngine &resources_;
  ResourceBindings &resource_bindings_;
  rcu::RcuDomain &rcu_;
  PortDirectory ports_;
};

}  // namespace framework
}  // namespace bess

#endif  // BESS_FRAMEWORK_MODULE_INIT_CONTEXT_H_
