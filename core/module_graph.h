// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULE_GRAPH_H_
#define BESS_MODULE_GRAPH_H_

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "runtime/runtime_state.h"
#include "gate.h"
#include "message.h"
#include "metadata.h"
#include "utils/common.h"

using bess::gate_idx_t;

class Module;
class ModuleBuilder;

// Manages a global graph of modules
class ModuleGraph {
 public:
  // Return true if any module from the builder exists
  static bool HasModuleOfClass(const ModuleBuilder *);

  // Creates a module.
  static Module *CreateModule(const ModuleBuilder &builder,
                              const std::string &module_name,
                              const google::protobuf::Any &arg,
                              pb_error_t *perr);

  // Removes a module from the runtime registry, destroys it and deletes it.
  static void DestroyModule(Module *m);
  static void DestroyAllModules();

  static int ConnectModules(Module *module, gate_idx_t ogate_idx,
                            Module *m_next, gate_idx_t igate_idx,
                            bool skip_default_hooks = false);
  static int DisconnectModule(Module *module, gate_idx_t ogate_idx);

  // Non-owning view of the runtime's module registry.
  static const bess::runtime::ModuleRegistry::Map &GetAllModules();

  static std::string GenerateDefaultName(const std::string &class_name,
                                         const std::string &default_template);

  // Updates the parents of tasks
  static void UpdateTaskGraph();

  // Cleans the parents of modules
  static void CleanTaskGraph();

  // Update information about what workers are accessing what module
  static void PropagateActiveWorker();

 private:
  static void UpdateParentsAs(Module *parent_task, Module *module,
                              std::unordered_set<Module *> &visited_modules);
  static void UpdateSingleTaskGraph(Module *module);

  static void PropagateIGatePriority(
      bess::IGate *igate, std::unordered_set<bess::IGate *> &visited_igate,
      uint32_t priority);

  static void SetIGatePriority(Module *task_module);
  static void SetUniqueGateIdx();
  static void ConfigureTasks();

  // Task membership, gate numbering and the dirty flag are graph state; module
  // instances themselves live in the runtime's ModuleRegistry.
  static uint32_t gate_cnt_;
  // Check if any changes on module graphs
  static bool changes_made_;
};

#endif
