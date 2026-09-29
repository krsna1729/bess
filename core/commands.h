// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2018, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_COMMANDS_H_
#define BESS_COMMANDS_H_

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
template <typename cmd_func_t>
struct GenericCommand {
  enum ThreadSafety { THREAD_UNSAFE = 0, THREAD_SAFE = 1 };

  std::string cmd;
  std::string arg_type;
  cmd_func_t func;

  // If set to THREAD_SAFE, workers don't need to be paused in order to run
  // this command.
  ThreadSafety mt_safe;
};

// Command and Commands are specifically *module* commands - these should
// be ModuleCommand and ModuleCommands, but they got there first.
using Command = GenericCommand<module_cmd_func_t>;
using Commands = std::vector<Command>;

using GateHookCommand = GenericCommand<gate_hook_cmd_func_t>;
using GateHookCommands = std::vector<GateHookCommand>;

#endif  // BESS_COMMANDS_H_
