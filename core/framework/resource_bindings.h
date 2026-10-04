// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_RESOURCE_BINDINGS_H_
#define BESS_FRAMEWORK_RESOURCE_BINDINGS_H_

#include <memory>
#include <unordered_map>
#include <utility>

#include "framework/resource_codec.h"

namespace bess::dataplane {
class Resource;
}  // namespace bess::dataplane

// Decision D-044 (docs/decisions.md): a dataplane Resource knows nothing about
// wire encoding. Control-side metadata (today the protobuf codec) is bound to
// a resource here, by the module that registers it, and read by the control
// plane to decode transactions and list resources.

namespace bess::framework {

class ResourceBindings;

// Owns one binding. Destroying or resetting it removes the binding, so a
// resource freed at the same address can never inherit a stale codec. Declare
// it after the resource it binds and reset it before the resource is.
class ResourceBinding {
 public:
  ResourceBinding() = default;
  ResourceBinding(ResourceBinding &&other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)),
        resource_(std::exchange(other.resource_, nullptr)) {}
  ResourceBinding &operator=(ResourceBinding &&other) noexcept {
    if (this != &other) {
      Reset();
      owner_ = std::exchange(other.owner_, nullptr);
      resource_ = std::exchange(other.resource_, nullptr);
    }
    return *this;
  }
  ResourceBinding(const ResourceBinding &) = delete;
  ResourceBinding &operator=(const ResourceBinding &) = delete;
  ~ResourceBinding() { Reset(); }

  void Reset();
  bool bound() const { return owner_ != nullptr; }

 private:
  friend class ResourceBindings;
  ResourceBinding(ResourceBindings *owner,
                  const dataplane::Resource *resource) noexcept
      : owner_(owner), resource_(resource) {}

  ResourceBindings *owner_ = nullptr;
  const dataplane::Resource *resource_ = nullptr;
};

// Not thread-safe: callers hold the control-plane lock, as module commands and
// the transaction RPC do.
class ResourceBindings {
 public:
  ResourceBindings() = default;
  ResourceBindings(const ResourceBindings &) = delete;
  ResourceBindings &operator=(const ResourceBindings &) = delete;

  // Binds `codec` to `resource`. A resource has one binding: binding it again
  // replaces the codec, and resetting either handle removes the binding. A
  // resource with no binding is not reachable over the RPC.
  [[nodiscard]] ResourceBinding Bind(
      const dataplane::Resource &resource,
      std::shared_ptr<const ResourceCodec> codec);

  // nullptr if `resource` is unbound. The pointer is valid until the binding
  // is reset.
  const ResourceCodec *Find(const dataplane::Resource &resource) const;

  size_t size() const { return codecs_.size(); }

  // The process's bindings. Process-scoped on purpose, like the transaction
  // engine and the control endpoint they describe: application instances
  // (M5) share them (consolidation review, 2026-10-04).
  static ResourceBindings &ProcessDefault();

 private:
  friend class ResourceBinding;
  void Unbind(const dataplane::Resource *resource);

  std::unordered_map<const dataplane::Resource *,
                     std::shared_ptr<const ResourceCodec>>
      codecs_;
};

class ModuleInitContext;
// The bindings an in-tree module attaches its resources' codecs to (D-044).
// Internal: codecs are protobuf-bound and not part of the plugin SDK.
ResourceBindings &BindingsOf(const ModuleInitContext &context) noexcept;

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_RESOURCE_BINDINGS_H_
