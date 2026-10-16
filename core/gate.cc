// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "gate.h"
#include "module.h"
#include "utils/format.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "worker.h"

namespace bess {

bool GateHookBuilder::RegisterGateHook(GateHook::constructor_t constructor,
                                       const std::string &class_name,
                                       const std::string &name_template,
                                       const std::string &help_text,
                                       GateHook::init_func_t init_func) {
  return all_gate_hook_builders_holder()
      .emplace(std::piecewise_construct, std::forward_as_tuple(class_name),
               std::forward_as_tuple(constructor, class_name, name_template,
                                     help_text, init_func))
      .second;
}

std::map<std::string, GateHookBuilder>
    &GateHookBuilder::all_gate_hook_builders_holder(bool reset) {
  // Maps from hook names to hook builders. Tracks all hooks (via their
  // GateHookBuilders).
  static std::map<std::string, GateHookBuilder> all_gate_hook_builders;

  if (reset) {
    all_gate_hook_builders.clear();
  }

  return all_gate_hook_builders;
}

const std::map<std::string, GateHookBuilder>
    &GateHookBuilder::all_gate_hook_builders() {
  return all_gate_hook_builders_holder();
}

const std::string Gate::GenerateDefaultName(const GateHookBuilder *builder,
                                            Gate *gate) {
  const std::string &name_template = builder->name_template();
  const std::vector<GateHook *> hooks = gate->hooks();
  for (int i = 0;; i++) {
    std::string name = name_template + std::to_string(i);
    if (std::none_of(hooks.begin(), hooks.end(), [&](const GateHook *hook) {
          return name == hook->name();
        })) {
      return name;
    }
  }
}

// Creates new gate hook from the given builder, initializes it,
// and adds it to the given gate.  If any of these steps go wrong
// the gatehook instance is destroyed and we return a failed
// CommandResponse, otherwise we return a successful one.
GateHook *Gate::CreateGateHook(const GateHookBuilder *builder, Gate *gate,
                               const std::string &name,
                               const google::protobuf::Any &arg,
                               pb_error_t *error) {
  bess::GateHook *hook = builder->hook_constructor_();
  CommandResponse init_ret = builder->hook_init_func_(hook, gate, arg);
  if (init_ret.error().code() != 0) {
    delete hook;
    *error = init_ret.error();
    return nullptr;
  }

  std::string hook_name = name;
  if (hook_name == "") {
    hook_name = GenerateDefaultName(builder, gate);
  }

  hook->set_class_name(builder->class_name());
  hook->set_name(hook_name);
  hook->set_arg(arg);
  hook->set_gate(gate);
  int ret = gate->AddHook(hook);
  if (ret != 0) {
    error->set_code(ret);
    error->set_errmsg("Fail to Add Hook");
    delete hook;
    return nullptr;
  }
  return hook;
}

int Gate::AddHook(GateHook *hook) {
  for (const auto &h : hooks_) {
    if (h->name() == hook->name()) {
      return EEXIST;
    }
  }

  hooks_.push_back(hook);

  const auto cmp = [](const GateHook *lhs, const GateHook *rhs) {
    return *lhs < *rhs;
  };
  std::sort(hooks_.begin(), hooks_.end(), cmp);

  return 0;
}

GateHook *Gate::FindHook(const std::string &name) {
  for (const auto &hook : hooks_) {
    if (hook->name() == name) {
      return hook;
    }
  }
  return nullptr;
}

void Gate::RemoveHook(const std::string &name) {
  for (auto it = hooks_.begin(); it != hooks_.end(); ++it) {
    GateHook *hook = *it;
    if (hook->name() == name) {
      delete hook;
      hooks_.erase(it);
      return;
    }
  }
}

GateHook *Gate::FindHookByClass(const std::string &class_name) {
  for (const auto &hook : hooks_) {
    if (hook->class_name() == class_name) {
      return hook;
    }
  }
  return nullptr;
}

void Gate::RemoveHookByClass(const std::string &class_name) {
  for (auto it = hooks_.begin(); it != hooks_.end(); ++it) {
    GateHook *hook = *it;
    if (hook->class_name() == class_name) {
      delete hook;
      hooks_.erase(it);
      return;
    }
  }
}

void Gate::ClearHooks() {
  for (auto &hook : hooks_) {
    delete hook;
  }
  hooks_.clear();
}

void IGate::PushOgate(OGate *og) {
  ogates_upstream_.push_back(og);
  mergeable_ = (ogates_upstream_.size() > 1);
}

void IGate::RemoveOgate(const OGate *og) {
  for (auto it = ogates_upstream_.begin(); it != ogates_upstream_.end(); ++it) {
    if (*it == og) {
      ogates_upstream_.erase(it);
      return;
    }
  }
  mergeable_ = (ogates_upstream_.size() > 1);
}

// Add internally-generated Track() hook to this ogate.
void OGate::AddTrackHook() {
  static const GateHookBuilder *track_builder;
  static google::protobuf::Any arg;

  // If we haven't located the track hook builder yet, do that first.
  if (track_builder == nullptr) {
    const auto it =
        bess::GateHookBuilder::all_gate_hook_builders().find(
            kTrackGateHookName);
    // Would like to use CHECK_NE here, but cannot because
    // operator<< is not defined on the arguments.
    if (it == bess::GateHookBuilder::all_gate_hook_builders().end()) {
      CHECK(0) << "Track gate hook builder is missing";
    }
    track_builder = &it->second;

    // Track takes a TrackArg; its byte-counting flag is reserved (D-140), so a
    // connected gate installs the default one.
    static bess::pb::TrackArg track_arg;
    CHECK(arg.PackFrom(track_arg));
  }
  pb_error_t error;
  this->CreateGateHook(track_builder, this, "", arg, &error);
}

void OGate::SetIgate(IGate *ig) {
  igate_ = ig;
  igate_idx_ = ig->gate_idx();
}

}  // namespace bess
