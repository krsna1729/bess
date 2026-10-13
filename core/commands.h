// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2018, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_COMMANDS_H_
#define BESS_COMMANDS_H_

#include <string>
#include <utility>
#include <vector>

#include "message.h"

// TODO(torek): refactor; instead of "module command" and "gate command"
// we should just have "command".  The constraint right now is that the
// commands to a module or gate-hook instance are redirected through
// the underlying builder (module) or factory (gate-hook), which
// get in the way here.

class Module;
namespace bess {
class GateHook;
};  // namespace bess

using module_cmd_func_t =
    pb_func_t<CommandResponse, Module, google::protobuf::Any>;
using gate_hook_cmd_func_t =
    pb_func_t<CommandResponse, bess::GateHook, google::protobuf::Any>;

// Describes a single command that can be issued to a module
// or gate hook (according to cmd_func_t).
//
// A module class's commands are its actions (D-129): operations that are not
// state -- lookups, statistics, clearing counters -- which control_v2's
// InvokeAction runs and GetSchema lists. A command that edits state a
// resource now holds is LEGACY_ONLY: the legacy ModuleCommand still runs it
// (pybess, .bess scripts), control_v2 neither lists nor runs it, and it
// leaves with the legacy service.
template <typename cmd_func_t>
struct GenericCommand {
  enum ThreadSafety { THREAD_UNSAFE = 0, THREAD_SAFE = 1 };
  enum Exposure { ACTION = 0, LEGACY_ONLY = 1 };

  std::string cmd;
  std::string arg_type;
  cmd_func_t func;

  // If set to THREAD_SAFE, workers don't need to be paused in order to run
  // this command.
  ThreadSafety mt_safe;

  // The type of the answer's data, resolved as arg_type is; "" when it
  // answers nothing.
  std::string result_type{};

  Exposure exposure = ACTION;
};

// Command and Commands are specifically *module* commands - these should
// be ModuleCommand and ModuleCommands, but they got there first.
using Command = GenericCommand<module_cmd_func_t>;
using Commands = std::vector<Command>;

using GateHookCommand = GenericCommand<gate_hook_cmd_func_t>;
using GateHookCommands = std::vector<GateHookCommand>;

// A resource table every module of a class registers as "<module>/<table>"
// (D-129), declared with the class so that GetSchema lists it and desired
// state naming it is checked before the module exists. A module that
// registers a table its class does not declare, or one of another kind or
// types, fails creation.
struct ResourceTable {
  enum Kind { MAP = 1, SINGLETON = 2 };  // TransactionResource.Kind's values

  std::string table;
  Kind kind;
  std::string key_type;  // protobuf full names; google.protobuf.Empty for a singleton
  std::string value_type;
  bool requires_paused_workers = false;

  template <typename KeyMsg, typename ValueMsg>
  static ResourceTable Map(std::string table, bool requires_paused_workers = false) {
    return {std::move(table), MAP, std::string(KeyMsg::descriptor()->full_name()),
            std::string(ValueMsg::descriptor()->full_name()), requires_paused_workers};
  }
  template <typename ValueMsg>
  static ResourceTable Singleton(std::string table, bool requires_paused_workers = false) {
    return {std::move(table), SINGLETON, "google.protobuf.Empty",
            std::string(ValueMsg::descriptor()->full_name()), requires_paused_workers};
  }
};
using ResourceTables = std::vector<ResourceTable>;

#endif  // BESS_COMMANDS_H_
