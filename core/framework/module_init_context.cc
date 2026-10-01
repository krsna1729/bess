// SPDX-License-Identifier: BSD-3-Clause

#include "framework/module_init_context.h"

#include "runtime/runtime_state.h"

namespace bess {
namespace framework {

Port *PortDirectory::Find(const std::string &name) const {
  return registry_.Find(name);
}

const ModuleInitContext &ModuleInitContext::ProcessDefault() {
  // Constructed after RuntimeState (first use), so destroyed before it.
  static const ModuleInitContext context(
      runtime::runtime().transactions(), runtime::runtime().rcu(),
      runtime::runtime().ports());
  return context;
}

}  // namespace framework
}  // namespace bess
