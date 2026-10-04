// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_RESOURCE_REGISTRY_H_
#define BESS_DATAPLANE_RESOURCE_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "dataplane/resource.h"

namespace bess::dataplane {

class TransactionEngine;

// What a module or library may do with the process's transactional resources
// (D-021, D-022): register the resources it owns, release them, and read
// reference counts. The public face of the transaction engine (consolidation
// review, 2026-10-04): applying transactions, reclamation and the engine's
// internals stay with the control plane. Callers hold the control-plane lock,
// as module commands do. Obtained from ModuleInitContext::resources() or, in
// tests, TransactionEngine::registry().
class ResourceRegistry {
 public:
  ResourceRegistry(const ResourceRegistry &) = delete;
  ResourceRegistry &operator=(const ResourceRegistry &) = delete;

  // See TransactionEngine::Register / Unregister / ReleaseForTeardown.
  std::expected<void, std::string> Register(Resource *resource);
  std::expected<void, std::string> Unregister(const std::string &name);
  std::expected<void, std::string> Unregister(std::span<const std::string> names);
  std::expected<void, std::string> ReleaseForTeardown(std::span<const std::string> names);

  // References to `key` of `resource` held by other resources' values.
  size_t ReferenceCount(const std::string &resource, const ResourceKey &key) const;
  // The generation of the last applied transaction.
  uint64_t generation() const;

 private:
  friend class TransactionEngine;
  friend TransactionEngine &EngineOf(ResourceRegistry &registry) noexcept;
  explicit ResourceRegistry(TransactionEngine &engine) noexcept : engine_(engine) {}

  TransactionEngine &engine_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_RESOURCE_REGISTRY_H_
