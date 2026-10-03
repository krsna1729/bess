// SPDX-License-Identifier: BSD-3-Clause

#include "framework/instance_registry.h"

#include <cxxabi.h>

#include <cstdlib>

#include "utils/logging.h"

namespace bess::framework {

const char *InstanceErrorName(InstanceError error) {
  switch (error) {
    case InstanceError::kExists:
      return "instance already exists";
    case InstanceError::kNotFound:
      return "instance not found";
    case InstanceError::kTypeMismatch:
      return "instance has a different type";
    case InstanceError::kInUse:
      return "instance is in use";
  }
  return "unknown instance error";
}

InstanceRegistry::~InstanceRegistry() {
  for (const auto &[name, entry] : instances_) {
    CHECK_EQ(0u, entry->leases)
        << "instance '" << name << "' destroyed with " << entry->leases
        << " outstanding lease(s)";
  }
}

std::expected<void, InstanceError> InstanceRegistry::Destroy(
    const std::string &name) {
  auto it = instances_.find(name);
  if (it == instances_.end()) {
    return std::unexpected(InstanceError::kNotFound);
  }
  if (it->second->leases != 0) {
    return std::unexpected(InstanceError::kInUse);
  }
  instances_.erase(it);
  return {};
}

std::vector<InstanceInfo> InstanceRegistry::Describe() const {
  std::vector<InstanceInfo> out;
  out.reserve(instances_.size());
  for (const auto &[name, entry] : instances_) {
    out.push_back(InstanceInfo{name, entry->type_name, entry->leases});
  }
  return out;
}

std::string InstanceRegistry::DemangledName(const std::type_info &type) {
  int status = 0;
  char *demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
  std::string name = status == 0 && demangled != nullptr ? demangled
                                                         : type.name();
  std::free(demangled);
  return name;
}

}  // namespace bess::framework
