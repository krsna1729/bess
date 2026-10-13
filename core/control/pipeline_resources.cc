// SPDX-License-Identifier: BSD-3-Clause

#include "control/pipeline_resources.h"

#include <algorithm>
#include <cerrno>
#include <string>
#include <unordered_map>
#include <utility>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message.h>

#include "framework/resource_codec.h"

namespace bess::control {

namespace {

using framework::ResourceCodec;

std::string CanonicalBytes(const google::protobuf::Message &message) {
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream out(&stream);
    out.SetSerializationDeterministic(true);
    // Fails only beyond protobuf's 2 GiB message limit.
    (void)message.SerializeToCodedStream(&out);
  }
  return bytes;
}

google::protobuf::Any Pack(const google::protobuf::Message &message) {
  google::protobuf::Any any;
  any.set_type_url("type.googleapis.com/" + std::string(message.GetDescriptor()->full_name()));
  any.set_value(CanonicalBytes(message));
  return any;
}

ControlError ResourceError(int err, const std::string &resource, const std::string &message) {
  ControlError error = Err(err, "%s", message.c_str());
  error.object = resource;
  error.field = "resources";
  return error;
}

ControlResult<const ResourceCodec *> CodecOf(const dataplane::TransactionEngine &engine,
                                             const framework::ResourceBindings &bindings,
                                             const std::string &name) {
  const dataplane::Resource *resource = engine.FindResource(name);
  const ResourceCodec *codec = resource == nullptr ? nullptr : bindings.Find(*resource);
  if (codec == nullptr) {
    return std::unexpected(
        ResourceError(ENOENT, name, "no resource '" + name + "' (ListTransactionResources lists them)"));
  }
  return codec;
}

}  // namespace

ControlResult<std::vector<ResourceSpec>> ReadResources(
    const dataplane::TransactionEngine &engine, const framework::ResourceBindings &bindings,
    std::span<const std::string> names) {
  std::vector<std::string> wanted(names.begin(), names.end());
  if (wanted.empty()) {
    for (const std::string &name : engine.ResourceNames()) {
      const dataplane::Resource *resource = engine.FindResource(name);
      if (resource != nullptr && bindings.Find(*resource) != nullptr) {
        wanted.push_back(name);
      }
    }
  }
  std::vector<ResourceSpec> out;
  out.reserve(wanted.size());
  for (const std::string &name : wanted) {
    auto codec = CodecOf(engine, bindings, name);
    if (!codec) {
      return std::unexpected(codec.error());
    }
    ResourceSpec spec;
    spec.name = name;
    const bool singleton = (*codec)->kind() == ResourceCodec::Kind::kSingleton;
    (*codec)->Entries([&](const google::protobuf::Message &key,
                          const google::protobuf::Message &value) {
      ResourceEntrySpec entry;
      if (!singleton) {
        entry.key = Pack(key);
      }
      entry.value = Pack(value);
      spec.entries.push_back(std::move(entry));
    });
    out.push_back(std::move(spec));
  }
  std::sort(out.begin(), out.end(),
            [](const ResourceSpec &a, const ResourceSpec &b) { return a.name < b.name; });
  return out;
}

ControlResult<ResourceConvergence> Converge(const dataplane::TransactionEngine &engine,
                                            const framework::ResourceBindings &bindings,
                                            const ResourceSpec &desired, bool build_ops) {
  auto found = CodecOf(engine, bindings, desired.name);
  if (!found) {
    return std::unexpected(found.error());
  }
  const ResourceCodec &codec = **found;
  const bool singleton = codec.kind() == ResourceCodec::Kind::kSingleton;
  ResourceConvergence result;
  result.delta.name = desired.name;
  result.requires_paused_workers = codec.requires_paused_workers();

  // Now: internal key -> canonical value bytes. A read-back key that does not
  // decode is the codec's bug, not the caller's.
  std::unordered_map<dataplane::ResourceKey, std::string> current;
  std::string unreadable;
  codec.Entries([&](const google::protobuf::Message &key, const google::protobuf::Message &value) {
    const google::protobuf::Any packed = Pack(key);
    auto internal = codec.Key(packed.type_url(), packed.value());
    if (!internal) {
      unreadable = internal.error();
      return;
    }
    current.emplace(std::move(*internal), CanonicalBytes(value));
  });
  if (!unreadable.empty()) {
    return std::unexpected(ResourceError(
        EIO, desired.name, "resource '" + desired.name + "' reads back a key it refuses: " + unreadable));
  }

  std::unordered_map<dataplane::ResourceKey, size_t> seen;  // key -> entry index
  for (size_t i = 0; i < desired.entries.size(); i++) {
    const ResourceEntrySpec &entry = desired.entries[i];
    const std::string where = "resource '" + desired.name + "' entry " + std::to_string(i);
    auto key = codec.Key(entry.key.type_url(), entry.key.value());
    if (!key) {
      return std::unexpected(ResourceError(EINVAL, desired.name, where + ": key: " + key.error()));
    }
    if (auto [it, fresh] = seen.emplace(*key, i); !fresh) {
      return std::unexpected(ResourceError(
          EINVAL, desired.name, where + ": the same key as entry " + std::to_string(it->second)));
    }
    const auto now = current.find(*key);
    if (now != current.end() && now->second == entry.value.value()) {
      continue;
    }
    (now == current.end() ? result.delta.created : result.delta.updated)++;
    if (build_ops) {
      auto value = codec.Value(entry.value.type_url(), entry.value.value());
      if (!value) {
        return std::unexpected(
            ResourceError(EINVAL, desired.name, where + ": value: " + value.error()));
      }
      result.ops.push_back(dataplane::Op::Upsert(desired.name, *key, std::move(*value)));
    }
  }
  if (!singleton) {
    for (const auto &[key, value] : current) {
      if (seen.contains(key)) {
        continue;
      }
      result.delta.removed++;
      if (build_ops) {
        result.ops.push_back(dataplane::Op::Erase(desired.name, key));
      }
    }
  }
  return result;
}

}  // namespace bess::control
