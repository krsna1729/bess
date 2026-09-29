// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_RESOURCE_CODEC_H_
#define BESS_DATAPLANE_RESOURCE_CODEC_H_

#include <any>
#include <expected>
#include <functional>
#include <string>
#include <utility>

#include <google/protobuf/any.pb.h>

#include "dataplane/resource.h"

namespace bess::dataplane {

// How a resource's keys and values arrive over the RPC (G1.2c, D-025): typed
// protobuf messages per resource, turned into the engine's ResourceKey and
// value here, so clients never reproduce BESS's internal key packing. A
// resource without a codec is not reachable over the RPC. Kept out of
// resource.h so the engine core does not depend on protobuf.
class ResourceCodec {
 public:
  virtual ~ResourceCodec() = default;

  // The protobuf full names of the key and value messages (for discovery).
  virtual std::string key_type() const = 0;
  virtual std::string value_type() const = 0;

  virtual std::expected<ResourceKey, std::string> Key(
      const google::protobuf::Any &key) const = 0;
  virtual std::expected<std::any, std::string> Value(
      const google::protobuf::Any &value) const = 0;
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
      const google::protobuf::Any &key) const override {
    KeyMsg msg;
    if (!key.UnpackTo(&msg)) {
      return std::unexpected("key is not a " + key_type());
    }
    return key_(msg);
  }
  std::expected<std::any, std::string> Value(
      const google::protobuf::Any &value) const override {
    ValueMsg msg;
    if (!value.UnpackTo(&msg)) {
      return std::unexpected("value is not a " + value_type());
    }
    return value_(msg);
  }

 private:
  KeyFn key_;
  ValueFn value_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_RESOURCE_CODEC_H_
