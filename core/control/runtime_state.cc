// Copyright (c) 2026, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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

#include "control/runtime_state.h"

#include <cctype>
#include <cerrno>
#include <sstream>

#include "module.h"
#include "port.h"
#include "control/worker_manager.h"
#include "traffic_class.h"
#include "utils/common.h"

namespace bess {
namespace control {

namespace {

// Both registries and both legacy helpers (PortBuilder::GenerateDefaultPortName,
// ModuleGraph::GenerateDefaultName) derive a name template from the type name
// when the type has no explicit template.
std::string NameTemplate(const std::string &class_name,
                         const std::string &default_template) {
  if (default_template != "") {
    return default_template;
  }

  std::ostringstream ss;
  char last_char = '\0';
  for (auto t : class_name) {
    if (last_char != '\0' && islower(last_char) && isupper(t)) {
      ss << '_';
    }

    ss << char(tolower(t));
    last_char = t;
  }
  return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// PortRegistry
// ---------------------------------------------------------------------------

Port *PortRegistry::Find(const std::string &name) const {
  auto it = ports_.find(name);
  return it == ports_.end() ? nullptr : it->second.get();
}

bool PortRegistry::Add(std::unique_ptr<Port> &&port) {
  if (ports_.count(port->name())) {
    return false;
  }
  ports_.emplace(port->name(), std::move(port));
  return true;
}

std::unique_ptr<Port> PortRegistry::Remove(const std::string &name) {
  auto it = ports_.find(name);
  if (it == ports_.end()) {
    return nullptr;
  }
  std::unique_ptr<Port> port = std::move(it->second);
  ports_.erase(it);
  return port;
}

int PortRegistry::Destroy(const std::string &name) {
  auto it = ports_.find(name);
  if (it == ports_.end()) {
    return -ENOENT;
  }

  Port *p = it->second.get();
  for (packet_dir_t dir : {PACKET_DIR_INC, PACKET_DIR_OUT}) {
    for (queue_t i = 0; i < p->num_queues[dir]; i++) {
      if (p->users[dir][i]) {
        return -EBUSY;
      }
    }
  }

  std::unique_ptr<Port> owned = std::move(it->second);
  ports_.erase(it);
  owned->DeInit();
  return 0;
}

std::string PortRegistry::GenerateDefaultName(
    const std::string &driver_name,
    const std::string &default_template) const {
  const std::string name_template =
      NameTemplate(driver_name, default_template);

  for (int i = 0;; i++) {
    std::ostringstream ss;
    ss << name_template << i;
    std::string name = ss.str();

    if (!ports_.count(name)) {
      return name;  // found an unallocated name!
    }
  }

  promise_unreachable();
}

void PortRegistry::Clear() {
  for (auto &pair : ports_) {
    pair.second->DeInit();
  }
  ports_.clear();
}

// ---------------------------------------------------------------------------
// ModuleRegistry
// ---------------------------------------------------------------------------

Module *ModuleRegistry::Find(const std::string &name) const {
  auto it = modules_.find(name);
  return it == modules_.end() ? nullptr : it->second.get();
}

bool ModuleRegistry::Add(std::unique_ptr<Module> &&module) {
  if (modules_.count(module->name())) {
    return false;
  }
  modules_.emplace(module->name(), std::move(module));
  return true;
}

std::unique_ptr<Module> ModuleRegistry::Remove(const std::string &name) {
  auto it = modules_.find(name);
  if (it == modules_.end()) {
    return nullptr;
  }
  std::unique_ptr<Module> module = std::move(it->second);
  modules_.erase(it);
  return module;
}

std::string ModuleRegistry::GenerateDefaultName(
    const std::string &class_name, const std::string &default_template) const {
  const std::string name_template = NameTemplate(class_name, default_template);

  for (int i = 0;; i++) {
    std::ostringstream ss;
    ss << name_template << i;
    std::string name = ss.str();

    if (!modules_.count(name)) {
      return name;
    }
  }

  promise_unreachable();
}

void ModuleRegistry::Clear() {
  for (auto &pair : modules_) {
    pair.second->Destroy();
  }
  modules_.clear();
  task_names_.clear();
}

// ---------------------------------------------------------------------------
// TrafficClassRegistry
// ---------------------------------------------------------------------------

bool TrafficClassRegistry::Register(std::unique_ptr<TrafficClass> &&c) {
  if (classes_.count(c->name())) {
    return false;
  }
  classes_.emplace(c->name(), std::move(c));
  return true;
}

TrafficClass *TrafficClassRegistry::Find(const std::string &name) const {
  auto it = classes_.find(name);
  return it == classes_.end() ? nullptr : it->second.get();
}

bool TrafficClassRegistry::Release(TrafficClass *c) {
  auto it = classes_.find(c->name());
  if (it == classes_.end() || it->second.get() != c) {
    return false;
  }
  // Forget the object without destroying it: the caller owns the teardown.
  it->second.release();
  classes_.erase(it);
  return true;
}

void TrafficClassRegistry::ReleaseTree(TrafficClass *root) {
  for (TrafficClass *child : root->Children()) {
    ReleaseTree(child);
  }
  Release(root);
}

void TrafficClassRegistry::ReleaseAll() {
  for (auto &pair : classes_) {
    pair.second.release();
  }
  classes_.clear();
}

// ---------------------------------------------------------------------------
// RuntimeState
// ---------------------------------------------------------------------------

RuntimeState::RuntimeState() : workers_(std::make_unique<WorkerManager>()) {}

RuntimeState::~RuntimeState() = default;

WorkerManager &RuntimeState::workers() {
  return *workers_;
}

RuntimeState &RuntimeState::Get() {
  static RuntimeState state;
  return state;
}

}  // namespace control
}  // namespace bess
