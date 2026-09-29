// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "resume_hook.h"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace bess {

std::set<std::unique_ptr<ResumeHook>, ResumeHook::UniquePtrLess>
    global_resume_hooks;

std::map<std::string, ResumeHookBuilder>
    &ResumeHookBuilder::all_resume_hook_builders_holder() {
  // Maps from hook names to hook builders. Tracks all hooks (via their
  // ResumeHookBuilders).
  static std::map<std::string, ResumeHookBuilder> all_resume_hook_builders;

  return all_resume_hook_builders;
}

const std::map<std::string, ResumeHookBuilder>
    &ResumeHookBuilder::all_resume_hook_builders() {
  return all_resume_hook_builders_holder();
}

bool ResumeHookBuilder::RegisterResumeHook(
    ResumeHook::constructor_t constructor, ResumeHook::init_func_t init_func,
    const std::string &hook_name) {
  return all_resume_hook_builders_holder()
      .emplace(std::piecewise_construct, std::forward_as_tuple(hook_name),
               std::forward_as_tuple(constructor, init_func, hook_name))
      .second;
}

void run_global_resume_hooks(bool run_modules) {
  auto &hooks = global_resume_hooks;

  for (auto &hook : hooks) {
    VLOG(1) << "Running global resume hook '" << hook->name() << "'";
    hook->Run();
  }

  if (run_modules) {
    auto &resume_modules = event_modules[Event::PreResume];
    for (auto it = resume_modules.begin(); it != resume_modules.end();) {
      int ret = (*it)->OnEvent(Event::PreResume);
      if (ret == -ENOTSUP) {
        it = resume_modules.erase(it);
      } else {
        it++;
      }
    }
  }
}

}  // namespace bess
