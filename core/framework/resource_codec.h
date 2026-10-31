// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_RESOURCE_CODEC_H_
#define BESS_FRAMEWORK_RESOURCE_CODEC_H_

#include <any>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <optional>
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
  // A visit that can end the walk: false stops it at the next position it
  // can resume from (a few more entries may be visited first).
  using PageVisitor = std::function<bool(const google::protobuf::Message &key,
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

  // A resumable walk, as Redis SCAN: visits committed entries from `cursor`
  // (empty: the first) until `visit` returns false or none remain, and
  // returns where to continue (empty: the walk is done). The cursor is the
  // codec's own bytes. An entry present for the whole walk is visited at
  // least once; one written or erased meanwhile may be missed or visited
  // twice, and a change of layout (a table that grows) may visit entries
  // twice. Called with the control-plane lock held, one call per page, so a
  // page costs its own entries rather than the resource's. A codec without
  // a position returns nullopt without visiting (read by key instead).
  virtual std::optional<std::string> VisitFrom(std::string_view /*cursor*/,
                                               const PageVisitor & /*visit*/) const {
    return std::nullopt;
  }

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

// A VisitFrom cursor as fixed-width numbers, then any bytes: a codec writes
// its position with CursorOf and reads it back with CursorFields, which is
// false for a cursor too short to be one (the empty one: start over).
template <size_t N>
std::string CursorOf(const std::array<uint32_t, N> &fields, std::string_view tail = {}) {
  std::string cursor(sizeof(fields), '\0');
  std::memcpy(cursor.data(), fields.data(), sizeof(fields));
  cursor.append(tail);
  return cursor;
}
template <size_t N>
bool CursorFields(std::string_view cursor, std::array<uint32_t, N> *fields,
                  std::string_view *tail = nullptr) {
  if (cursor.size() < sizeof(*fields)) {
    return false;
  }
  cursor.copy(reinterpret_cast<char *>(fields->data()), sizeof(*fields));
  if (tail != nullptr) {
    *tail = cursor.substr(sizeof(*fields));
  }
  return true;
}

// A map resource's codec from conversion functions over concrete message
// types: key and value to the engine's representation, and the committed
// entries back to messages -- a whole walk (EntriesFn), or a resumable one
// (PageFn, ResourceCodec::VisitFrom's contract) that also serves Entries.
// A resource that can hold session-scale entries gives a PageFn, so a paged
// read costs a page per call. `worker_running`, if set, makes the resource
// one that requires paused workers (see ResourceCodec).
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
  using TypedPageVisitor = std::function<bool(const KeyMsg &, const ValueMsg &)>;
  using PageFn =
      std::function<std::string(std::string_view cursor, const TypedPageVisitor &)>;

  TypedCodec(KeyFn key, ValueFn value, EntriesFn entries,
             WorkerRunning worker_running = nullptr)
      : key_(std::move(key)),
        value_(std::move(value)),
        entries_(std::move(entries)),
        worker_running_(std::move(worker_running)) {}
  TypedCodec(KeyFn key, ValueFn value, PageFn page, WorkerRunning worker_running = nullptr)
      : key_(std::move(key)),
        value_(std::move(value)),
        page_(std::move(page)),
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
    if (page_ != nullptr) {
      (void)page_({}, [&visit](const KeyMsg &key, const ValueMsg &value) {
        visit(key, value);
        return true;
      });
      return;
    }
    entries_([&visit](const KeyMsg &key, const ValueMsg &value) { visit(key, value); });
  }

  std::optional<std::string> VisitFrom(std::string_view cursor,
                                       const PageVisitor &visit) const override {
    if (page_ == nullptr) {
      return std::nullopt;
    }
    return page_(cursor, [&visit](const KeyMsg &key, const ValueMsg &value) {
      return visit(key, value);
    });
  }

  bool requires_paused_workers() const override { return worker_running_ != nullptr; }
  bool Busy() const override { return worker_running_ != nullptr && worker_running_(); }

 private:
  KeyFn key_;
  ValueFn value_;
  EntriesFn entries_;
  PageFn page_;
  WorkerRunning worker_running_;
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_RESOURCE_CODEC_H_
