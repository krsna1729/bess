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

namespace bess::dataplane {

// Maps a serialized resource key/value message to the engine's internal
// representation. Control-plane transports adapt their wire envelope to this
// interface; module authors provide only typed conversion functions.
class ResourceCodec {
 public:
  virtual ~ResourceCodec() = default;

  // The protobuf full names of the key and value messages (for discovery).
  virtual std::string key_type() const = 0;
  virtual std::string value_type() const = 0;

  virtual std::expected<ResourceKey, std::string> Key(
      std::string_view type_url, const std::string &serialized) const = 0;
  virtual std::expected<std::any, std::string> Value(
      std::string_view type_url, const std::string &serialized) const = 0;
};

// A codec from two conversion functions over concrete message types.
template <typename KeyMsg, typename ValueMsg>
class TypedCodec final : public ResourceCodec {
 public:
  using KeyFn =
      std::function<std::expected<ResourceKey, std::string>(const KeyMsg &)>;
  using ValueFn =
      std::function<std::expected<std::any, std::string>(const ValueMsg &)>;

  TypedCodec(KeyFn key, ValueFn value)
      : key_(std::move(key)), value_(std::move(value)) {}

  std::string key_type() const override {
    return std::string(KeyMsg::descriptor()->full_name());
  }
  std::string value_type() const override {
    return std::string(ValueMsg::descriptor()->full_name());
  }

  std::expected<ResourceKey, std::string> Key(
      std::string_view type_url, const std::string &serialized) const override {
    KeyMsg msg;
    if (!Unpack(type_url, serialized, key_type(), &msg)) {
      return std::unexpected("key is not a " + key_type());
    }
    return key_(msg);
  }
  std::expected<std::any, std::string> Value(
      std::string_view type_url, const std::string &serialized) const override {
    ValueMsg msg;
    if (!Unpack(type_url, serialized, value_type(), &msg)) {
      return std::unexpected("value is not a " + value_type());
    }
    return value_(msg);
  }

 private:
  template <typename Message>
  static bool Unpack(std::string_view type_url, const std::string &serialized,
                     const std::string &full_name, Message *message) {
    const size_t slash = type_url.rfind('/');
    if (slash == std::string_view::npos ||
        type_url.substr(slash + 1) != full_name) {
      return false;
    }
    return message->ParseFromString(serialized);
  }

  KeyFn key_;
  ValueFn value_;
};

}  // namespace bess::dataplane

#endif  // BESS_FRAMEWORK_RESOURCE_CODEC_H_
