// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_RESOURCE_CODEC_H_
#define BESS_FRAMEWORK_RESOURCE_CODEC_H_

#include <any>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <google/protobuf/message.h>

#include "dataplane/resource.h"

namespace bess::framework {

// Maps a serialized resource key/value message to the engine's internal
// representation, and reads the resource's contents back as messages.
// Control-plane transports adapt their wire envelope to this interface;
// module authors provide only typed conversion functions.
class ResourceCodec {
 public:
  // D-129: a map holds entries by key; a singleton holds one value under the
  // empty key, which an upsert sets and an erase cannot remove.
  enum class Kind : uint8_t { kMap = 1, kSingleton = 2 };

  // A committed entry, as messages: (key, value). A singleton's key is a
  // google.protobuf.Empty.
  using EntryVisitor = std::function<void(const google::protobuf::Message &key,
                                          const google::protobuf::Message &value)>;
  // Whether a worker runs the resource's module now (for a resource whose
  // values its workers read without synchronization).
  using WorkerRunning = std::function<bool()>;

  virtual ~ResourceCodec() = default;

  virtual Kind kind() const = 0;

  // The protobuf full names of the key and value messages (for discovery).
  virtual std::string key_type() const = 0;
  virtual std::string value_type() const = 0;

  virtual std::expected<dataplane::ResourceKey, std::string> Key(
      std::string_view type_url, const std::string &serialized) const = 0;
  virtual std::expected<std::any, std::string> Value(
      std::string_view type_url, const std::string &serialized) const = 0;

  // Every committed entry, in any order (desired state reads resources back
  // with it: GetPipeline, and the diff an apply converges by). What it yields
  // must decode through Key and Value to the same entry. Called with the
  // control-plane lock held.
  virtual void Entries(const EntryVisitor &visit) const = 0;

  // A resource that requires paused workers changes state its module's
  // workers read without synchronization: the control plane refuses a
  // transaction naming it while Busy() (a worker runs the module), and pauses
  // the workers for a desired-state apply that changes it.
  virtual bool requires_paused_workers() const { return false; }
  virtual bool Busy() const { return false; }
};

namespace internal {

// Unpacks `serialized` as `full_name` if `type_url` names it.
template <typename Message>
bool UnpackAs(std::string_view type_url, const std::string &serialized,
              std::string_view full_name, Message *message) {
  const size_t slash = type_url.rfind('/');
  if (slash == std::string_view::npos || type_url.substr(slash + 1) != full_name) {
    return false;
  }
  return message->ParseFromString(serialized);
}

}  // namespace internal

// A map resource's codec from conversion functions over concrete message
// types: key and value to the engine's representation, and the committed
// entries back to messages. `worker_running`, if set, makes the resource one
// that requires paused workers (see ResourceCodec).
template <typename KeyMsg, typename ValueMsg>
class TypedCodec final : public ResourceCodec {
 public:
  using KeyFn =
      std::function<std::expected<dataplane::ResourceKey, std::string>(
          const KeyMsg &)>;
  using ValueFn =
      std::function<std::expected<std::any, std::string>(const ValueMsg &)>;
  using TypedVisitor = std::function<void(const KeyMsg &, const ValueMsg &)>;
  using EntriesFn = std::function<void(const TypedVisitor &)>;

  TypedCodec(KeyFn key, ValueFn value, EntriesFn entries,
             WorkerRunning worker_running = nullptr)
      : key_(std::move(key)),
        value_(std::move(value)),
        entries_(std::move(entries)),
        worker_running_(std::move(worker_running)) {}

  Kind kind() const override { return Kind::kMap; }
  std::string key_type() const override {
    return std::string(KeyMsg::descriptor()->full_name());
  }
  std::string value_type() const override {
    return std::string(ValueMsg::descriptor()->full_name());
  }

  std::expected<dataplane::ResourceKey, std::string> Key(
      std::string_view type_url, const std::string &serialized) const override {
    KeyMsg msg;
    if (!internal::UnpackAs(type_url, serialized, KeyMsg::descriptor()->full_name(), &msg)) {
      return std::unexpected("key is not a " + key_type());
    }
    return key_(msg);
  }
  std::expected<std::any, std::string> Value(
      std::string_view type_url, const std::string &serialized) const override {
    ValueMsg msg;
    if (!internal::UnpackAs(type_url, serialized, ValueMsg::descriptor()->full_name(), &msg)) {
      return std::unexpected("value is not a " + value_type());
    }
    return value_(msg);
  }

  void Entries(const EntryVisitor &visit) const override {
    entries_([&visit](const KeyMsg &key, const ValueMsg &value) { visit(key, value); });
  }

  bool requires_paused_workers() const override { return worker_running_ != nullptr; }
  bool Busy() const override { return worker_running_ != nullptr && worker_running_(); }

 private:
  KeyFn key_;
  ValueFn value_;
  EntriesFn entries_;
  WorkerRunning worker_running_;
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_RESOURCE_CODEC_H_
