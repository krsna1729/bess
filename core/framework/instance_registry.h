// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_INSTANCE_REGISTRY_H_
#define BESS_FRAMEWORK_INSTANCE_REGISTRY_H_

#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

// Decision D-045 (docs/decisions.md): application instances.
//
// An application (a vSwitch, a UPF, ...) owns an object graph that several
// module instances share. An InstanceRegistry holds those objects by name with
// an explicit structural lifecycle, so the framework offers no implicit
// "service" any module can conjure up:
//
//   - Create() makes one; a duplicate name is an error, never a lookup.
//   - Lookup() never creates; a missing name or a different type is an error.
//   - Destroy() is explicit and is refused while any lease is outstanding.
//   - The packet path touches none of this: a module resolves its instance
//     once, in Init(), keeps the lease, and caches `lease.get()`. No name
//     lookup, no reference count, and no registry access per packet.
//
// Creation and destruction are structural operations: run them under the
// control-plane lock, with the pipeline quiescent for the consumers being
// changed. The instance's contents may update live under whatever
// synchronization the instance itself provides. The registry is not
// thread-safe; callers hold the control-plane lock, as module commands do.

namespace bess::framework {

enum class InstanceError {
  kExists,        // an instance with that name already exists
  kNotFound,      // no instance with that name
  kTypeMismatch,  // the instance has a different type
  kInUse,         // leases are outstanding
};

const char *InstanceErrorName(InstanceError error);

struct InstanceInfo {
  std::string name;
  std::string type;  // demangled C++ type name
  size_t leases;     // outstanding leases
};

class InstanceRegistry;

namespace internal {
struct InstanceEntry {
  std::type_index type;
  std::string type_name;
  std::unique_ptr<void, void (*)(void *)> object;
  size_t leases = 0;
};
}  // namespace internal

// A counted borrow of an instance. While any lease exists the instance cannot
// be destroyed, so a pointer cached from get() stays valid for the lease's
// lifetime. Holding a lease is control-plane bookkeeping; copying the cached
// pointer is free and reading it never consults the registry.
template <typename T>
class InstanceLease {
 public:
  InstanceLease() = default;
  InstanceLease(InstanceLease &&other) noexcept
      : object_(std::exchange(other.object_, nullptr)),
        entry_(std::exchange(other.entry_, nullptr)) {}
  InstanceLease &operator=(InstanceLease &&other) noexcept {
    if (this != &other) {
      Release();
      object_ = std::exchange(other.object_, nullptr);
      entry_ = std::exchange(other.entry_, nullptr);
    }
    return *this;
  }
  InstanceLease(const InstanceLease &) = delete;
  InstanceLease &operator=(const InstanceLease &) = delete;
  ~InstanceLease() { Release(); }

  T *get() const { return object_; }
  T *operator->() const { return object_; }
  T &operator*() const { return *object_; }
  explicit operator bool() const { return object_ != nullptr; }

  void Release() {
    if (entry_ != nullptr) {
      entry_->leases--;
      entry_ = nullptr;
      object_ = nullptr;
    }
  }

 private:
  friend class InstanceRegistry;
  InstanceLease(T *object, internal::InstanceEntry *entry)
      : object_(object), entry_(entry) {
    entry_->leases++;
  }

  T *object_ = nullptr;
  internal::InstanceEntry *entry_ = nullptr;
};

class InstanceRegistry {
 public:
  InstanceRegistry() = default;
  InstanceRegistry(const InstanceRegistry &) = delete;
  InstanceRegistry &operator=(const InstanceRegistry &) = delete;
  // Every lease must be gone: a module still holding one would dangle.
  ~InstanceRegistry();

  // Constructs a T named `name` and returns a lease on it.
  template <typename T, typename... Args>
  std::expected<InstanceLease<T>, InstanceError> Create(const std::string &name,
                                                        Args &&...args) {
    if (instances_.contains(name)) {
      return std::unexpected(InstanceError::kExists);
    }
    T *raw = new T(std::forward<Args>(args)...);
    auto entry = std::make_unique<internal::InstanceEntry>(
        internal::InstanceEntry{
            std::type_index(typeid(T)), DemangledName(typeid(T)),
            std::unique_ptr<void, void (*)(void *)>(
                raw, [](void *p) { delete static_cast<T *>(p); }),
            0});
    internal::InstanceEntry *e = entry.get();
    instances_.emplace(name, std::move(entry));
    return InstanceLease<T>(raw, e);
  }

  // Borrows the existing instance `name` as a T. Never creates.
  template <typename T>
  std::expected<InstanceLease<T>, InstanceError> Lookup(
      const std::string &name) {
    auto it = instances_.find(name);
    if (it == instances_.end()) {
      return std::unexpected(InstanceError::kNotFound);
    }
    internal::InstanceEntry *e = it->second.get();
    if (e->type != std::type_index(typeid(T))) {
      return std::unexpected(InstanceError::kTypeMismatch);
    }
    return InstanceLease<T>(static_cast<T *>(e->object.get()), e);
  }

  // Destroys `name`. Refused with kInUse while any lease is outstanding.
  std::expected<void, InstanceError> Destroy(const std::string &name);

  // Name, type and lease count of every instance, in name order.
  std::vector<InstanceInfo> Describe() const;

  size_t size() const { return instances_.size(); }

 private:
  static std::string DemangledName(const std::type_info &type);

  std::map<std::string, std::unique_ptr<internal::InstanceEntry>> instances_;
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_INSTANCE_REGISTRY_H_
