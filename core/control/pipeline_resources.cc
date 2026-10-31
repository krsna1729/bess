// SPDX-License-Identifier: BSD-3-Clause

#include "control/pipeline_resources.h"

#include <algorithm>
#include <cerrno>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message.h>

#include "framework/resource_codec.h"

namespace bess::control {

namespace {

using framework::ResourceCodec;

// Sized exactly: a grown string keeps up to twice its bytes, per entry read.
// Into `bytes`, so a caller that keeps few of them reuses one buffer.
void CanonicalBytes(const google::protobuf::Message &message, std::string *bytes) {
  bytes->resize(message.ByteSizeLong());
  google::protobuf::io::ArrayOutputStream stream(bytes->data(), static_cast<int>(bytes->size()));
  google::protobuf::io::CodedOutputStream out(&stream);
  out.SetSerializationDeterministic(true);
  message.SerializeWithCachedSizes(&out);
}

std::string CanonicalBytes(const google::protobuf::Message &message) {
  std::string bytes;
  CanonicalBytes(message, &bytes);
  return bytes;
}

std::string TypeUrl(std::string_view full_name) {
  return "type.googleapis.com/" + std::string(full_name);
}

google::protobuf::Any Pack(const google::protobuf::Message &message, std::string bytes) {
  google::protobuf::Any any;
  any.set_type_url(TypeUrl(message.GetDescriptor()->full_name()));
  any.set_value(std::move(bytes));
  return any;
}

google::protobuf::Any Pack(const google::protobuf::Message &message) {
  return Pack(message, CanonicalBytes(message));
}

// A length-delimited field of `bytes` behind a one-byte tag.
size_t FieldBytes(size_t bytes) {
  return 1 + google::protobuf::io::CodedOutputStream::VarintSize64(bytes) + bytes;
}

// The entry as one element of a repeated ResourceEntry field (a
// ResourceContents' or a ResourcePage's): its key when set, its value always.
size_t FramedEntryBytes(const ResourceEntrySpec &entry) {
  size_t bytes = FieldBytes(entry.value.ByteSizeLong());
  if (!entry.key.type_url().empty()) {
    bytes += FieldBytes(entry.key.ByteSizeLong());
  }
  return FieldBytes(bytes);
}

// The smallest entry the codec's types allow: empty key and value messages.
size_t MinEntryBytes(const ResourceCodec &codec) {
  ResourceEntrySpec entry;
  if (codec.kind() != ResourceCodec::Kind::kSingleton) {
    entry.key.set_type_url(TypeUrl(codec.key_type()));
  }
  entry.value.set_type_url(TypeUrl(codec.value_type()));
  return FramedEntryBytes(entry);
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

ControlError TooLarge(size_t bytes, size_t max_bytes) {
  ControlError error = Err(EMSGSIZE,
                           "the resources are at least %zu bytes, above the %zu-byte message "
                           "limit: name fewer resources, or read one with ReadResource",
                           bytes, max_bytes);
  error.code = ControlErrorCode::kResourceExhausted;
  error.field = "resources";
  return error;
}

}  // namespace

ControlResult<std::vector<ResourceSpec>> ReadResources(
    const dataplane::TransactionEngine &engine, const framework::ResourceBindings &bindings,
    std::span<const std::string> names, size_t max_bytes) {
  std::vector<std::string> wanted(names.begin(), names.end());
  if (wanted.empty()) {
    for (const std::string &name : engine.ResourceNames()) {
      const dataplane::Resource *resource = engine.FindResource(name);
      if (resource != nullptr && bindings.Find(*resource) != nullptr) {
        wanted.push_back(name);
      }
    }
  }
  // Refused before reading an entry when even the smallest entries the types
  // allow do not fit: the walk would build that much and more.
  std::vector<const ResourceCodec *> codecs;
  codecs.reserve(wanted.size());
  size_t smallest = 0;
  for (const std::string &name : wanted) {
    auto codec = CodecOf(engine, bindings, name);
    if (!codec) {
      return std::unexpected(codec.error());
    }
    smallest += engine.FindResource(name)->LiveCount() * MinEntryBytes(**codec);
    codecs.push_back(*codec);
  }
  if (smallest > max_bytes) {
    return std::unexpected(TooLarge(smallest, max_bytes));
  }
  std::vector<ResourceSpec> out;
  out.reserve(wanted.size());
  size_t bytes = 0;
  for (size_t i = 0; i < wanted.size(); i++) {
    ResourceSpec spec;
    spec.name = wanted[i];
    spec.entries.reserve(engine.FindResource(wanted[i])->LiveCount());
    const bool singleton = codecs[i]->kind() == ResourceCodec::Kind::kSingleton;
    codecs[i]->Entries([&](const google::protobuf::Message &key,
                           const google::protobuf::Message &value) {
      if (bytes > max_bytes) {
        return;  // refused: build no more
      }
      ResourceEntrySpec entry;
      if (!singleton) {
        entry.key = Pack(key);
      }
      entry.value = Pack(value);
      bytes += FramedEntryBytes(entry);
      spec.entries.push_back(std::move(entry));
    });
    if (bytes > max_bytes) {
      return std::unexpected(TooLarge(bytes, max_bytes));
    }
    out.push_back(std::move(spec));
  }
  std::sort(out.begin(), out.end(),
            [](const ResourceSpec &a, const ResourceSpec &b) { return a.name < b.name; });
  return out;
}

ControlResult<ResourceEntryPage> ReadResourcePage(
    const dataplane::TransactionEngine &engine, const framework::ResourceBindings &bindings,
    const std::string &name, const std::string &cursor, size_t max_entries, size_t max_bytes) {
  auto codec = CodecOf(engine, bindings, name);
  if (!codec) {
    return std::unexpected(codec.error());
  }
  const bool singleton = (*codec)->kind() == ResourceCodec::Kind::kSingleton;
  ResourceEntryPage page;
  size_t bytes = 0;
  // The codec's own position: the page costs its entries.
  std::optional<std::string> next = (*codec)->VisitFrom(
      cursor, [&](const google::protobuf::Message &key, const google::protobuf::Message &value) {
        ResourceEntrySpec entry;
        if (!singleton) {
          entry.key = Pack(key);
        }
        entry.value = Pack(value);
        bytes += FramedEntryBytes(entry);
        page.entries.push_back(std::move(entry));
        return page.entries.size() < max_entries && bytes < max_bytes;
      });
  if (next.has_value()) {
    if (!next->empty()) {
      page.next = std::move(*next);
    }
    return page;
  }

  // scale: no position, so every page walks the whole resource -- a full
  // read is ceil(N / max_entries) walks, O(N^2 / page) visits. Bounded only
  // for resources a module keeps small (configuration, not sessions); a
  // codec that can hold session-scale N gives TypedCodec a PageFn instead.
  // The cursor is "k" and the last key's canonical bytes (never empty, so
  // an empty key still continues); the page is the smallest keys after it
  // that fit, as a max-heap by key: an entry that sorts after a full page's
  // last is never packed, and one that pushes the page over evicts the
  // largest.
  std::optional<std::string_view> after;
  if (!cursor.empty()) {
    after = std::string_view(cursor).substr(1);
  }
  struct Candidate {
    std::string key;  // canonical bytes
    ResourceEntrySpec entry;
    size_t bytes;
  };
  const auto by_key = [](const Candidate &a, const Candidate &b) { return a.key < b.key; };
  std::vector<Candidate> heap;
  bool more = false;
  std::string canonical;  // reused: most keys a walk sees are not kept
  (*codec)->Entries([&](const google::protobuf::Message &key,
                        const google::protobuf::Message &value) {
    CanonicalBytes(key, &canonical);
    if (after.has_value() && canonical <= *after) {
      return;
    }
    if (!heap.empty() && canonical > heap.front().key &&
        (heap.size() >= max_entries || bytes >= max_bytes)) {
      more = true;
      return;
    }
    Candidate candidate{canonical, {}, 0};
    if (!singleton) {
      candidate.entry.key = Pack(key, candidate.key);
    }
    candidate.entry.value = Pack(value);
    candidate.bytes = FramedEntryBytes(candidate.entry);
    bytes += candidate.bytes;
    heap.push_back(std::move(candidate));
    std::push_heap(heap.begin(), heap.end(), by_key);
    while (heap.size() > max_entries || (bytes > max_bytes && heap.size() > 1)) {
      std::pop_heap(heap.begin(), heap.end(), by_key);
      bytes -= heap.back().bytes;
      heap.pop_back();
      more = true;
    }
  });
  std::sort_heap(heap.begin(), heap.end(), by_key);
  page.entries.reserve(heap.size());
  for (Candidate &candidate : heap) {
    page.entries.push_back(std::move(candidate.entry));
  }
  if (more) {
    page.next = "k" + heap.back().key;
  }
  return page;
}

ControlResult<ResourceClearBatch> ClearBatch(const dataplane::TransactionEngine &engine,
                                             const framework::ResourceBindings &bindings,
                                             const std::string &name, const std::string &cursor,
                                             size_t max_ops) {
  auto found = CodecOf(engine, bindings, name);
  if (!found) {
    return std::unexpected(found.error());
  }
  const ResourceCodec &codec = **found;
  ResourceClearBatch batch;
  if (max_ops == 0 || codec.kind() == ResourceCodec::Kind::kSingleton) {
    return batch;
  }
  batch.requires_paused_workers = codec.requires_paused_workers();
  batch.ops.reserve(max_ops);
  // A read-back key that does not decode is the codec's bug (as Converge).
  std::string unreadable;
  const auto erase = [&](const google::protobuf::Message &key) {
    const google::protobuf::Any packed = Pack(key);
    auto internal = codec.Key(packed.type_url(), packed.value());
    if (!internal) {
      unreadable = internal.error();
      return false;
    }
    batch.ops.push_back(dataplane::Op::Erase(name, std::move(*internal)));
    return batch.ops.size() < max_ops;
  };
  std::optional<std::string> next = codec.VisitFrom(
      cursor, [&](const google::protobuf::Message &key, const google::protobuf::Message &) {
        return erase(key);
      });
  if (!next.has_value()) {
    // scale: no position, so each batch walks the whole resource for its
    // first max_ops keys: a clear is ceil(N / max_ops) walks, O(N^2 / batch)
    // visits. Bounded only for resources a module keeps small; one that can
    // hold session-scale N has a position (TypedCodec's PageFn). The cursor
    // only says that keys remain: the erased ones are gone.
    bool more = false;
    codec.Entries([&](const google::protobuf::Message &key, const google::protobuf::Message &) {
      if (more || !unreadable.empty()) {
        return;
      }
      if (batch.ops.size() >= max_ops) {
        more = true;
        return;
      }
      (void)erase(key);
    });
    next = more ? "k" : "";
  }
  if (!unreadable.empty()) {
    return std::unexpected(ResourceError(
        EIO, name, "resource '" + name + "' reads back a key it refuses: " + unreadable));
  }
  if (!next->empty()) {
    batch.next = std::move(*next);
  }
  return batch;
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
