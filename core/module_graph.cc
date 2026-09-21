// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "module_graph.h"

#include <glog/logging.h>

#include "gate.h"
#include "gate_hooks/track.h"
#include "module.h"
#include "scheduler.h"
#include "utils/extended_priority_queue.h"

bool ModuleGraph::changes_made_ = false;
uint32_t ModuleGraph::gate_cnt_;

struct IGateGreater {
  bool operator()(const bess::IGate *left, const bess::IGate *right) const {
    return left->priority() > right->priority();
  }
};

void ModuleGraph::UpdateParentsAs(
    Module *task, Module *module,
    std::unordered_set<Module *> &visited_modules) {
  visited_modules.insert(module);

  if (module->is_task()) {
    module->AddParentTask(task);
    return;
  } else {
    std::vector<bess::OGate *> ogates = module->ogates();
    for (size_t i = 0; i < ogates.size(); i++) {
      if (!ogates[i]) {
        continue;
      }
      Module *child = ogates[i]->igate()->module();
      if (visited_modules.count(child) != 0) {
        continue;
      }
      UpdateParentsAs(task, child, visited_modules);
    }
  }
}

void ModuleGraph::UpdateSingleTaskGraph(Module *task_module) {
  std::unordered_set<Module *> visited_modules;
  visited_modules.insert(task_module);

  std::vector<bess::OGate *> ogates = task_module->ogates();
  for (size_t i = 0; i < ogates.size(); i++) {
    if (!ogates[i]) {
      continue;
    }

    Module *child = ogates[i]->igate()->module();
    if (visited_modules.count(child) != 0) {
      continue;
    }

    UpdateParentsAs(task_module, child, visited_modules);
  }
}

void ModuleGraph::PropagateIGatePriority(
    bess::IGate *igate, std::unordered_set<bess::IGate *> &visited_igates,
    uint32_t priority) {
  if (igate->module()->is_task()) {
    return;
  } else {
    std::vector<bess::OGate *> ogates = igate->module()->ogates();
    for (size_t i = 0; i < ogates.size(); i++) {
      if (!ogates[i]) {
        continue;
      }

      bess::IGate *next_igate = ogates[i]->igate();
      if (visited_igates.count(next_igate) != 0 ||  // This is a loop or
          next_igate->priority() >= priority) {     // visited by longer path
        continue;
      }

      visited_igates.insert(next_igate);
      next_igate->SetPriority(priority);
      PropagateIGatePriority(next_igate, visited_igates, priority + 1);
      visited_igates.erase(next_igate);
    }
  }
}

void ModuleGraph::SetIGatePriority(Module *task_module) {
  uint32_t priority = 1;
  std::unordered_set<bess::IGate *> visited_igates;

  std::vector<bess::OGate *> ogates = task_module->ogates();
  for (size_t i = 0; i < ogates.size(); i++) {
    if (!ogates[i]) {
      continue;
    }

    bess::IGate *igate = ogates[i]->igate();
    if (visited_igates.count(igate) != 0 ||  // This is a loop or
        igate->priority() >= priority) {     // visited by longer path
      continue;
    }

    visited_igates.insert(igate);
    igate->SetPriority(priority);
    PropagateIGatePriority(igate, visited_igates, priority + 1);
    visited_igates.erase(igate);
  }
}

void ModuleGraph::SetUniqueGateIdx() {
  bess::utils::extended_priority_queue<bess::OGate *> ogates_queue;
  bess::utils::extended_priority_queue<bess::IGate *, IGateGreater>
      igates_queue;
  std::unordered_set<bess::IGate *> igates_pushed;

  for (auto const &e : bess::control::runtime().modules().All()) {
    std::vector<bess::OGate *> ogates = e.second->ogates();
    for (size_t i = 0; i < ogates.size(); i++) {
      if (!ogates[i]) {
        continue;
      }

      ogates_queue.push(ogates[i]);

      bess::IGate *igate = ogates[i]->igate();
      if (igates_pushed.count(igate) != 0) {
        continue;
      }

      igates_pushed.insert(igate);
      igates_queue.push(igate);
    }
  }

  gate_cnt_ = 0;
  while (!igates_queue.empty()) {
    bess::IGate *igate = igates_queue.top();
    igates_queue.pop();

    igate->SetUniqueIdx(gate_cnt_++);
  }

  while (!ogates_queue.empty()) {
    bess::OGate *ogate = ogates_queue.top();
    ogates_queue.pop();

    ogate->SetUniqueIdx(gate_cnt_++);
  }
}

void ModuleGraph::ConfigureTasks() {
  for (int i = 0; i < Worker::kMaxWorkers; i++) {
    if (workers[i] == nullptr) {
      continue;
    }

    for (const auto &tc_pair : bess::TrafficClassBuilder::all_tcs()) {
      bess::TrafficClass *c = tc_pair.second.get();
      if (c->policy() == bess::POLICY_LEAF) {
        auto leaf = static_cast<bess::LeafTrafficClass *>(c);
        leaf->task()->UpdatePerGateBatch(gate_cnt_);
      }
    }
  }
}

void ModuleGraph::UpdateTaskGraph() {
  if (!changes_made_) {
    return;
  }

  // Do not change order here

  CleanTaskGraph();

  for (auto const &task : bess::control::runtime().modules().TaskNames()) {
    Module *m = bess::control::runtime().modules().Find(task);
    if (m) {
      UpdateSingleTaskGraph(m);
      SetIGatePriority(m);
    }
  }

  SetUniqueGateIdx();
  ConfigureTasks();

  changes_made_ = false;
}

void ModuleGraph::CleanTaskGraph() {
  for (auto const &task : bess::control::runtime().modules().TaskNames()) {
    Module *m = bess::control::runtime().modules().Find(task);
    if (m) {
      m->ClearParentTasks();
    }
  }
}

const bess::control::ModuleRegistry::Map &ModuleGraph::GetAllModules() {
  return bess::control::runtime().modules().All();
}

bool ModuleGraph::HasModuleOfClass(const ModuleBuilder *builder) {
  for (auto const &e : bess::control::runtime().modules().All()) {
    if (e.second->module_builder() == builder) {
      return true;
    }
  }
  return false;
}

// Creates a module to the graph.
Module *ModuleGraph::CreateModule(const ModuleBuilder &builder,
                                  const std::string &module_name,
                                  const google::protobuf::Any &arg,
                                  pb_error_t *perr) {
  std::unique_ptr<Module> m(
      builder.CreateModule(module_name, &bess::metadata::default_pipeline));

  CommandResponse ret = m->InitWithGenericArg(arg);
  {
    google::protobuf::Any empty;

    if (ret.data().SerializeAsString() != empty.SerializeAsString()) {
      LOG(WARNING) << module_name << "::" << builder.class_name()
                   << " Init() returned non-empty response: "
                   << ret.data().DebugString();
    }
  }

  if (ret.error().code() != 0) {
    *perr = ret.error();
    // Ideally these would be in the module destructors,
    // but there are many modules (find RegisterTask calls
    // in core/modules/*) that create tasks for themselves
    // and do not have their own destructors.  For now,
    // just call the teardown code here on failure.
    m->Destroy();
    return nullptr;
  }

  if (m->is_task()) {
    if (!bess::control::runtime().modules().MarkTask(m->name())) {
      *perr = pb_errno(ENOMEM);
      m->Destroy();
      return nullptr;
    }
  }

  Module *raw = m.get();
  if (!bess::control::runtime().modules().Add(std::move(m))) {
    *perr = pb_errno(ENOMEM);
    bess::control::runtime().modules().UnmarkTask(module_name);
    raw->Destroy();
    return nullptr;
  }

  return raw;
}

void ModuleGraph::DestroyModule(Module *m) {
  changes_made_ = true;

  if (m->is_task()) {
    bess::control::runtime().modules().UnmarkTask(m->name());
  }

  std::unique_ptr<Module> owned = bess::control::runtime().modules().Remove(m->name());
  if (!owned) {
    // Not registered (test-only path): keep the legacy behavior of destroying
    // and deleting what the caller handed over.
    owned.reset(m);
  }

  owned->Destroy();
}

void ModuleGraph::DestroyAllModules() {
  changes_made_ = true;

  bess::control::runtime().modules().Clear();
}

int ModuleGraph::ConnectModules(Module *module, gate_idx_t ogate_idx,
                                Module *m_next, gate_idx_t igate_idx,
                                bool skip_default_hooks) {
  if (ogate_idx >= module->module_builder()->NumOGates() ||
      ogate_idx >= MAX_GATES) {
    return -EINVAL;
  }

  if (igate_idx >= m_next->module_builder()->NumIGates() ||
      igate_idx >= MAX_GATES) {
    return -EINVAL;
  }

  changes_made_ = true;

  int ret = module->ConnectGate(ogate_idx, m_next, igate_idx);
  if (ret != 0)
    return ret;

  if (!skip_default_hooks) {
    // Gate tracking is enabled by default
    module->ogates()[ogate_idx]->AddTrackHook();
  }

  return 0;
}

int ModuleGraph::DisconnectModule(Module *module, gate_idx_t ogate_idx) {
  if (ogate_idx >= module->module_builder()->NumOGates()) {
    return -EINVAL;
  }

  changes_made_ = true;

  module->DisconnectGate(ogate_idx);

  return 0;
}

std::string ModuleGraph::GenerateDefaultName(
    const std::string &class_name, const std::string &default_template) {
  std::string name_template;

  if (default_template == "") {
    std::ostringstream ss;
    char last_char = '\0';
    for (auto t : class_name) {
      if (last_char != '\0' && islower(last_char) && isupper(t))
        ss << '_';

      ss << char(tolower(t));
      last_char = t;
    }
    name_template = ss.str();
  } else {
    name_template = default_template;
  }

  for (int i = 0;; i++) {
    std::ostringstream ss;
    ss << name_template << i;
    std::string name = ss.str();

    if (!bess::control::runtime().modules().Contains(name))
      return name;
  }

  promise_unreachable();
}

void ModuleGraph::PropagateActiveWorker() {
  for (auto &pair : bess::control::runtime().modules().All()) {
    Module *m = pair.second.get();
    m->ResetActiveWorkerSet();
  }
  for (int i = 0; i < Worker::kMaxWorkers; i++) {
    if (workers[i] == nullptr) {
      continue;
    }
    if (bess::TrafficClass *root = workers[i]->scheduler()->root()) {
      for (const auto &tc_pair : bess::TrafficClassBuilder::all_tcs()) {
        bess::TrafficClass *c = tc_pair.second.get();
        if (c->policy() == bess::POLICY_LEAF && c->Root() == root) {
          auto leaf = static_cast<bess::LeafTrafficClass *>(c);
          leaf->task()->AddActiveWorker(i);
        }
      }
    }
  }
}
