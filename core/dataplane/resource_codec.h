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
