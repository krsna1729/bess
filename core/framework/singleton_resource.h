// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_SINGLETON_RESOURCE_H_
#define BESS_FRAMEWORK_SINGLETON_RESOURCE_H_

#include <any>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <google/protobuf/empty.pb.h>

#include "dataplane/resource.h"
#include "dataplane/resource_registry.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "utils/logging.h"

// Singleton resources (D-129): one typed value per module -- its run-time
// settings ("<module>/config") -- changed through the same transactions as
// map resources. Experimental, installed with the codec types.
//
// A module declares the table with its class
// (ResourceTable::Singleton<ValueMsg>("config", paused)), keeps a Singleton
// member, and registers it in Init():
//
//   config_.Register(init_context().resources(), init_context().codecs(),
//                    name() + "/config",
//                    [](const pb::SourceConfig &v) -> std::expected<Settings, std::string> {...},
//                    [this](Settings &s) { burst_.store(s.burst); },
//                    [this] { pb::SourceConfig v; ...; return v; },
//                    nullptr);  // or [this] { return HasRunningWorker(); }
//
//   - stage: checks a value and builds everything applying it needs (it may
//     allocate and fail); a refused value refuses the transaction.
//   - apply: makes a staged value the module's. It runs in the publication
//     phase and must neither fail nor allocate: move or store only.
//   - read: the current value, for desired state (GetPipeline).
//   - worker_running: null if apply is safe while workers run (atomic
//     stores, or a pointer swap readers bind once per batch). Otherwise a
//     predicate the control plane checks: a transaction naming the resource
//     while it is true is refused, and a desired-state apply pauses the
//     workers for it.

namespace bess::framework {

namespace internal {

template <typename Staged>
class SingletonResource final : public dataplane::Resource {
 public:
  using ApplyFn = std::function<void(Staged &, dataplane::Retirer &)>;

  SingletonResource(std::string name, ApplyFn apply, Footprint footprint,
                    ResourceCodec::WorkerRunning worker_running)
      : Resource(std::move(name)),
        apply_(std::move(apply)),
        footprint_(footprint),
        worker_running_(std::move(worker_running)) {}

  bool Contains(const dataplane::ResourceKey &key) const override { return key.empty(); }

  std::expected<Reservation, std::string> Reserve(const dataplane::Op &op) override {
    if (!op.key.empty()) {
      return std::unexpected("a singleton takes no key");
    }
    if (op.kind == dataplane::OpKind::kErase) {
      return std::unexpected("a singleton is set, not erased");
    }
    if (worker_running_ != nullptr && worker_running_()) {
      return std::unexpected("a worker runs the module: pause the workers first");
    }
    const auto *staged = std::any_cast<std::shared_ptr<Staged>>(&op.value);
    if (staged == nullptr || *staged == nullptr) {
      return std::unexpected("wrong value type");
    }
    Reservation reservation{std::make_unique<Set>(*this, *staged), {}, footprint_};
    reservation.existed = true;
    return reservation;
  }

  size_t LiveCount() const override { return 0; }

 private:
  class Set final : public dataplane::StagedOp {
   public:
    Set(SingletonResource &owner, std::shared_ptr<Staged> staged)
        : owner_(owner), staged_(std::move(staged)) {}
    void Publish(dataplane::Retirer &retirer) noexcept override {
      owner_.apply_(*staged_, retirer);
    }

   private:
    SingletonResource &owner_;
    std::shared_ptr<Staged> staged_;
  };

  ApplyFn apply_;
  Footprint footprint_;
  ResourceCodec::WorkerRunning worker_running_;
};

template <typename ValueMsg, typename Staged>
class SingletonCodec final : public ResourceCodec {
 public:
  using StageFn = std::function<std::expected<Staged, std::string>(const ValueMsg &)>;
  using ReadFn = std::function<ValueMsg()>;

  SingletonCodec(StageFn stage, ReadFn read, WorkerRunning worker_running)
      : stage_(std::move(stage)),
        read_(std::move(read)),
        worker_running_(std::move(worker_running)) {}

  Kind kind() const override { return Kind::kSingleton; }
  std::string key_type() const override {
    return std::string(google::protobuf::Empty::descriptor()->full_name());
  }
  std::string value_type() const override {
    return std::string(ValueMsg::descriptor()->full_name());
  }

  // No key (an unset Any), or an empty google.protobuf.Empty.
  std::expected<dataplane::ResourceKey, std::string> Key(
      std::string_view type_url, const std::string &serialized) const override {
    if (type_url.empty() && serialized.empty()) {
      return dataplane::ResourceKey();
    }
    google::protobuf::Empty empty;
    if (!UnpackAs(type_url, serialized, google::protobuf::Empty::descriptor()->full_name(),
                  &empty) ||
        !serialized.empty()) {
      return std::unexpected("a singleton takes no key");
    }
    return dataplane::ResourceKey();
  }

  std::expected<std::any, std::string> Value(std::string_view type_url,
                                             const std::string &serialized) const override {
    ValueMsg msg;
    if (!UnpackAs(type_url, serialized, ValueMsg::descriptor()->full_name(), &msg)) {
      return std::unexpected("value is not a " + value_type());
    }
    auto staged = stage_(msg);
    if (!staged) {
      return std::unexpected(std::move(staged.error()));
    }
    return std::any(std::make_shared<Staged>(std::move(*staged)));
  }

  void Entries(const EntryVisitor &visit) const override {
    visit(google::protobuf::Empty::default_instance(), read_());
  }

  bool requires_paused_workers() const override { return worker_running_ != nullptr; }
  bool Busy() const override { return worker_running_ != nullptr && worker_running_(); }

 private:
  StageFn stage_;
  ReadFn read_;
  WorkerRunning worker_running_;
};

}  // namespace internal

// A module's singleton resource: the dataplane resource and its codec,
// registered together and released together (in DeInit, or on destruction).
// Not thread-safe: the control-plane lock, as for every resource.
template <typename ValueMsg, typename Staged>
class Singleton {
 public:
  using StageFn = typename internal::SingletonCodec<ValueMsg, Staged>::StageFn;
  using ReadFn = typename internal::SingletonCodec<ValueMsg, Staged>::ReadFn;
  // Publishes a staged value; replaced objects go to the Retirer (within the
  // footprint Register declared), freed a grace period later.
  using RetiringApplyFn = typename internal::SingletonResource<Staged>::ApplyFn;
  // Publishes a staged value that replaces nothing a reader may hold.
  using ApplyFn = std::function<void(Staged &)>;

  Singleton() = default;
  Singleton(const Singleton &) = delete;
  Singleton &operator=(const Singleton &) = delete;
  ~Singleton() { Release(); }

  // Registers `name` ("<module>/<table>") and binds its codec. Refused: a
  // taken name, or a second registration.
  std::expected<void, std::string> Register(dataplane::ResourceRegistry &registry,
                                            ResourceBindings &bindings, std::string name,
                                            StageFn stage, ApplyFn apply, ReadFn read,
                                            ResourceCodec::WorkerRunning worker_running) {
    return Register(
        registry, bindings, std::move(name), std::move(stage),
        [apply = std::move(apply)](Staged &staged, dataplane::Retirer &) { apply(staged); },
        dataplane::Resource::Footprint{}, std::move(read), std::move(worker_running));
  }

  // The same for an apply that swaps an object readers may still hold (an
  // RCU-published settings block) and retires the old one: `footprint` bounds
  // what one apply asks of the Retirer (Resource::Footprint; e.g. one retire).
  std::expected<void, std::string> Register(dataplane::ResourceRegistry &registry,
                                            ResourceBindings &bindings, std::string name,
                                            StageFn stage, RetiringApplyFn apply,
                                            dataplane::Resource::Footprint footprint,
                                            ReadFn read,
                                            ResourceCodec::WorkerRunning worker_running) {
    if (resource_ != nullptr) {
      return std::unexpected("singleton '" + resource_->name() + "' is registered already");
    }
    auto resource = std::make_unique<internal::SingletonResource<Staged>>(
        std::move(name), std::move(apply), footprint, worker_running);
    ResourceBinding binding = bindings.Bind(
        *resource, std::make_shared<internal::SingletonCodec<ValueMsg, Staged>>(
                       std::move(stage), std::move(read), std::move(worker_running)));
    if (auto registered = registry.Register(resource.get()); !registered) {
      return std::unexpected(registered.error());
    }
    registry_ = &registry;
    resource_ = std::move(resource);
    binding_ = std::move(binding);
    return {};
  }

  // Unregisters and unbinds; nothing if not registered. Idempotent.
  void Release() {
    if (resource_ == nullptr) {
      return;
    }
    const std::string name = resource_->name();
    auto released = registry_->ReleaseForTeardown(std::span<const std::string>(&name, 1));
    CHECK(released) << released.error();
    binding_.Reset();
    resource_.reset();
    registry_ = nullptr;
  }

  bool registered() const { return resource_ != nullptr; }

 private:
  dataplane::ResourceRegistry *registry_ = nullptr;
  std::unique_ptr<internal::SingletonResource<Staged>> resource_;
  ResourceBinding binding_;  // after resource_ (D-044)
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_SINGLETON_RESOURCE_H_
