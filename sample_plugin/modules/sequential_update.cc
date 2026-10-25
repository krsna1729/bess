// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "sequential_update.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <iterator>
#include <string>

#include "framework/plugin.h"
#include "packet_pool.h"
#include "packet_reshape.h"

using bess::utils::be16_t;
using bess::utils::be32_t;

// D-140: every legacy command is gone. "<name>/fields" holds the field list
// (the whole list, as SequentialUpdateArg); the creation argument is
// GetPipeline's Module.arg.
const Commands SequentialUpdate::cmds = {};

const ResourceTables SequentialUpdate::resource_tables = {
    ResourceTable::Singleton<sample::supdate::pb::SequentialUpdateArg>("fields",
                                                                       true)};

CommandResponse
SequentialUpdate::Init(const sample::supdate::pb::SequentialUpdateArg &arg) {
  CommandResponse ret = AddFields(arg);
  if (ret.has_error()) {
    return ret;
  }

  // Stage converts the whole list; apply copies it in (no allocation).
  // ProcessBatch reads and writes vars_ in place: paused workers only.
  auto registered = fields_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/fields",
      [](const sample::supdate::pb::SequentialUpdateArg &v)
          -> std::expected<Staged, std::string> {
        if (static_cast<size_t>(v.fields_size()) > kMaxVariable) {
          return std::unexpected("max " + std::to_string(kMaxVariable) +
                                 " variables can be specified");
        }
        Staged staged;
        for (int i = 0; i < v.fields_size(); i++) {
          CommandResponse r = ParseField(v.fields(i), &staged.vars[i]);
          if (r.has_error()) {
            return std::unexpected(r.error().errmsg());
          }
          // ParseField bounds offset + size by the data room: no overflow.
          staged.required_len =
              std::max(staged.required_len,
                       staged.vars[i].offset + staged.vars[i].size);
        }
        staged.num_vars = v.fields_size();
        staged.arg = v;
        return staged;
      },
      [this](Staged &s) {
        std::copy(std::begin(s.vars), std::end(s.vars), std::begin(vars_));
        num_vars_ = s.num_vars;
        required_len_ = s.required_len;
        arg_.Swap(&s.arg);
      },
      [this] { return arg_; }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void SequentialUpdate::DeInit() { fields_resource_.Release(); }

CommandResponse SequentialUpdate::ParseField(
    const sample::supdate::pb::SequentialUpdateArg::Field &var, Var *out) {
  const size_t size = var.size();
  uint32_t min = static_cast<uint32_t>(var.min());
  uint32_t max = static_cast<uint32_t>(var.max());

  switch (size) {
  case 1:
    min = std::min(min, static_cast<uint32_t>(0xff));
    max = std::min(max, static_cast<uint32_t>(0xff));
    break;

  case 2:
    min = std::min(min, static_cast<uint32_t>(0xffff));
    max = std::min(max, static_cast<uint32_t>(0xffff));
    break;

  case 4:
    break;

  default:
    return CommandFailure(EINVAL, "'size' must be 1, 2, or 4");
  }

  // The field must fit in one segment of the packets the dataplane
  // allocates; whether a given packet's first segment holds it is checked
  // per packet. A negative offset would name bytes before the packet's first
  // byte. Compare against the room left, never `offset + size`, which
  // overflows for an offset near INT64_MAX.
  const int64_t offset = var.offset();
  const size_t room = bess::PacketPool::DefaultDataRoomSize();
  if (offset < 0) {
    return CommandFailure(EINVAL, "'offset' must not be negative");
  }
  if (size > room || static_cast<uint64_t>(offset) > room - size) {
    return CommandFailure(EINVAL,
                          "too large 'offset': offset + size must be at most "
                          "the %zu-byte packet data room",
                          room);
  }

  if (min > max) {
    return CommandFailure(EINVAL, "'min' should not be greater than 'max'");
  }

  out->offset = static_cast<size_t>(offset);
  out->size = static_cast<uint8_t>(size);
  out->min = min;

  // avoid modulo 0
  out->range = (max - min + 1) ?: 0xffffffff;
  out->cur = 0;
  return CommandSuccess();
}

CommandResponse SequentialUpdate::AddFields(
    const sample::supdate::pb::SequentialUpdateArg &arg) {
  size_t curr = num_vars_;
  if (curr + arg.fields_size() > kMaxVariable) {
    return CommandFailure(EINVAL, "max %zu variables can be specified",
                          kMaxVariable);
  }

  size_t required = required_len_;
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &vars_[curr + i]);
    if (ret.has_error()) {
      return ret;
    }
    // ParseField bounds offset + size by the data room: no overflow.
    required =
        std::max(required, vars_[curr + i].offset + vars_[curr + i].size);
  }

  num_vars_ = curr + arg.fields_size();
  required_len_ = required;
  arg_.mutable_fields()->MergeFrom(arg.fields());
  return CommandSuccess();
}

// A packet that is too short in its first segment -- a runt, or a chain whose
// first segment ends before a field -- is dropped rather than partly written.
// A shared one (a clone) is copied first, so the payload's other holders
// never see the write; WritableHead checks the length again on the copy, and
// a packet whose copy fails is dropped.
void SequentialUpdate::PrepareBatch(Context *ctx, bess::PacketBatch *batch) {
  const size_t required = required_len_;
  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    bess::PacketHandle handle = pkt.handle();
    if (!bess::packet::WritableHead(handle, required)) {
      EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
      continue;
    }
    batch->handles()[out_cnt++] = handle;
  }
  batch->set_cnt(out_cnt);
}

namespace {

// Writes the next values of `var`'s sequence into every packet's `size`-byte
// field, big-endian, and returns where the sequence stopped.
template <size_t size, typename Var>
inline uint32_t WriteField(const Var &var, bess::PacketBatch *batch) {
  uint32_t cur = var.cur;
  for (bess::PacketRef pkt : batch->packets()) {
    const uint32_t value = var.min + cur;
    cur = cur + 1;
    if (cur >= var.range) {
      cur = 0;
    }

    char *field = pkt.head_data<char *>() + var.offset;
    if constexpr (size == 1) {
      *field = static_cast<char>(value);
    } else if constexpr (size == 2) {
      const be16_t v(static_cast<uint16_t>(value));
      std::memcpy(field, &v, sizeof(v));
    } else {
      const be32_t v(value);
      std::memcpy(field, &v, sizeof(v));
    }
  }
  return cur;
}

} // namespace

void SequentialUpdate::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (num_vars_ != 0) {
    // Every field is written into the packet's first segment, so that
    // segment must hold the furthest byte any field names, and its payload
    // must be this packet's alone. One pass that only reads tests every
    // packet first: a batch whose packets are all long enough and unshared
    // (the common case) is written as it is, and any other batch goes to
    // PrepareBatch.
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

  for (size_t i = 0; i < num_vars_; i++) {
    Var &var = vars_[i];
    switch (var.size) {
    case 1:
      var.cur = WriteField<1>(var, batch);
      break;
    case 2:
      var.cur = WriteField<2>(var, batch);
      break;
    default:
      var.cur = WriteField<4>(var, batch);
      break;
    }
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(SequentialUpdate, "supdate",
           "updates packet data sequentially in a range")

BESS_PLUGIN_REQUIRES("sample_sequential_update", "1.0.0",
                     BESS_CAP_INIT_CONTEXT | BESS_CAP_RESOURCES |
                         BESS_CAP_EVENTS | BESS_CAP_EVENT_SOURCES |
                         BESS_CAP_TICKS)
