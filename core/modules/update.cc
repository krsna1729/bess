// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "update.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <iterator>
#include <string>

#include "utils/endian.h"

// "<name>/fields" holds the whole field list; the class has no actions.
const Commands Update::cmds = {};

const ResourceTables Update::resource_tables = {
    ResourceTable::Singleton<bess::pb::UpdateArg>("fields", true)};

CommandResponse Update::Init(const bess::pb::UpdateArg &arg) {
  // The init argument's fields; a refused one refuses the creation. The same
  // validation the resource uses: a list longer than fields_ is refused
  // instead of walking past the array.
  CommandResponse ret = ParseFields(arg, fields_, &num_fields_);
  if (ret.has_error()) {
    return ret;
  }
  arg_ = arg;

  // Stage converts the whole list; apply copies it in (no allocation).
  // ProcessBatch reads fields_ in place: paused workers only.
  auto registered = fields_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/fields",
      [](const bess::pb::UpdateArg &v) -> std::expected<Staged, std::string> {
        Staged staged;
        CommandResponse r = ParseFields(v, staged.fields, &staged.num_fields);
        if (r.has_error()) {
          return std::unexpected(r.error().errmsg());
        }
        staged.arg = v;
        return staged;
      },
      [this](Staged &s) {
        std::copy(std::begin(s.fields), std::end(s.fields),
                  std::begin(fields_));
        num_fields_ = s.num_fields;
        arg_.Swap(&s.arg);
      },
      [this] { return arg_; }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Update::DeInit() {
  fields_resource_.Release();
}

void Update::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (size_t i = 0; i < num_fields_; i++) {
    const Field &field = fields_[i];
    const size_t offset = field.offset;
    const uint8_t size = field.size;

    for (bess::PacketRef snb : batch->packets()) {
      char *head = snb.head_data<char *>();

      // The value is big-endian with the field's bytes at the top of the
      // word, so its first `size` bytes are the field. Writing exactly those
      // bytes leaves the neighbours alone and stays inside the data room
      // ParseField validated; the eight-byte load/store this replaces could
      // run past it for a field narrower than eight bytes.
      std::memcpy(head + offset, &field.value, size);
    }
  }

  RunNextModule(ctx, batch);
}

CommandResponse Update::ParseFields(const bess::pb::UpdateArg &arg,
                                    Field *fields, size_t *num_fields) {
  if (static_cast<size_t>(arg.fields_size()) > kMaxFields) {
    return CommandFailure(EINVAL, "max %zu variables can be specified",
                          kMaxFields);
  }
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &fields[i]);
    if (ret.has_error()) {
      return ret;
    }
  }
  *num_fields = arg.fields_size();
  return CommandSuccess();
}

CommandResponse Update::ParseField(const bess::pb::UpdateArg::Field &field,
                                   Field *out) {
  const size_t size = field.size();
  if (size < 1 || size > 8) {
    return CommandFailure(EINVAL, "'size' must be 1-8");
  }

  // The top of the range is the data room. The bottom is the packet's first
  // byte: a negative offset would name bytes before it, whose bound depends
  // on the packet's current headroom and is not checked per packet.
  const int64_t offset = field.offset();
  if (offset < 0) {
    return CommandFailure(EINVAL, "'offset' must not be negative");
  }
  // Compare against the room left, never `offset + size`: the sum overflows
  // int64_t for an offset near INT64_MAX, which would pass the check and
  // leave ProcessBatch a wild pointer.
  if (offset > static_cast<int64_t>(SNBUF_DATA) - static_cast<int64_t>(size)) {
    return CommandFailure(EINVAL, "too large 'offset'");
  }

  // The value must fit in `size` bytes. Shifting it into the top of the word
  // first would drop any higher bits silently, so check the width it came
  // with.
  if (size < 8 && (field.value() >> (size * 8)) != 0) {
    return CommandFailure(
        EINVAL, "'value' field has not a correct %zu-byte value", size);
  }

  out->offset = static_cast<size_t>(offset);
  out->size = static_cast<uint8_t>(size);
  out->value = be64_t(field.value() << ((8 - size) * 8));
  return CommandSuccess();
}

ADD_MODULE(Update, "update", "updates packet data with specified values")
