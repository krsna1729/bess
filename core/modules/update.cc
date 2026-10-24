// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "update.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <iterator>
#include <string>

#include "packet_mutation.h"
#include "packet_reshape.h"
#include "utils/endian.h"

// "<name>/fields" holds the whole field list; the class has no actions.
const Commands Update::cmds = {};

const ResourceTables Update::resource_tables = {
    ResourceTable::Singleton<bess::pb::UpdateArg>("fields", true)};

CommandResponse Update::Init(const bess::pb::UpdateArg &arg) {
  // The init argument's fields; a refused one refuses the creation. The same
  // validation the resource uses: a list longer than fields_ is refused
  // instead of walking past the array.
  CommandResponse ret = ParseFields(arg, fields_, &num_fields_, &required_len_);
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
        CommandResponse r = ParseFields(v, staged.fields, &staged.num_fields,
                                        &staged.required_len);
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
        required_len_ = s.required_len;
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

// A packet that is too short in its first segment -- a runt, or a chain whose
// first segment ends before a field -- is dropped rather than partly written
// (D-172's packet destinations). A shared one is copied first; the copy may
// replace the mbuf and resegment it, so the length is checked again on it.
void Update::PrepareBatch(Context *ctx, bess::PacketBatch *batch) {
  const size_t required = required_len_;
  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    if (bess::packet::HeadWritableInPlace(pkt, required)) {
      batch->handles()[out_cnt++] = pkt.handle();
      continue;
    }
    bess::PacketHandle handle = pkt.handle();
    if (pkt.head_len() < required || !bess::packet::EnsureWritable(handle) ||
        bess::PacketRef(handle).head_len() < required) {
      EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
      continue;
    }
    batch->handles()[out_cnt++] = handle;
  }
  batch->set_cnt(out_cnt);
}

void Update::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (num_fields_ != 0) {
    // One pass that only reads, and one branch for the batch: a batch whose
    // packets are all long enough and unshared (the common case) is written
    // as it is. Every field is written into the packet's first segment, so
    // that segment must hold the furthest byte any field names, and its
    // payload must be this packet's alone (D-153): a clone's or a shared
    // external buffer's bytes are seen by every holder. HeadWritableInPlace
    // tests the common case only (a direct mbuf held once), without a branch;
    // anything else (including an indirect mbuf or external buffer that
    // happens to be writable) is decided by PrepareBatch.
    const size_t required = required_len_;
    unsigned ready = 1;
    for (bess::PacketRef pkt : batch->packets()) {
      ready &= static_cast<unsigned>(
          bess::packet::HeadWritableInPlace(pkt, required));
    }
    if (unlikely(ready == 0)) {
      PrepareBatch(ctx, batch);
    }
  }

  for (size_t i = 0; i < num_fields_; i++) {
    const Field &field = fields_[i];
    const size_t offset = field.offset;
    const uint8_t size = field.size;

    for (bess::PacketRef snb : batch->packets()) {
      char *head = snb.head_data<char *>();

      // The value is big-endian with the field's bytes at the top of the
      // word, so its first `size` bytes are the field. Writing exactly those
      // bytes leaves the neighbours alone; HeadWritableInPlace or
      // PrepareBatch keeps them inside the first segment's data.
      std::memcpy(head + offset, &field.value, size);
    }
  }

  RunNextModule(ctx, batch);
}

CommandResponse Update::ParseFields(const bess::pb::UpdateArg &arg,
                                    Field *fields, size_t *num_fields,
                                    size_t *required_len) {
  if (static_cast<size_t>(arg.fields_size()) > kMaxFields) {
    return CommandFailure(EINVAL, "max %zu variables can be specified",
                          kMaxFields);
  }
  size_t required = 0;
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &fields[i]);
    if (ret.has_error()) {
      return ret;
    }
    // ParseField bounds offset + size by the data room: no overflow.
    required = std::max(required, fields[i].offset + fields[i].size);
  }
  *num_fields = arg.fields_size();
  *required_len = required;
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
