// SPDX-License-Identifier: BSD-3-Clause

#include "framework/module_init_context.h"

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "runtime/runtime_state.h"

namespace bess {
namespace framework {

Port *PortDirectory::Find(const std::string &name) const {
  return registry_.Find(name);
}

const ModuleInitContext &ModuleInitContext::ProcessDefault() {
  // Never destroyed: modules the runtime owns call back into it while the
  // runtime itself is being destroyed at exit.
  static const ModuleInitContext *const context = new ModuleInitContext(
      runtime::runtime().transactions().registry(), ResourceBindings::ProcessDefault(),
      runtime::runtime().instances(), runtime::runtime().rcu(),
      runtime::runtime().ports(), runtime::runtime().metrics());
  return *context;
}

ResourceBindings &BindingsOf(const ModuleInitContext &context) noexcept {
  return context.resource_bindings_;
}

}  // namespace framework
}  // namespace bess
