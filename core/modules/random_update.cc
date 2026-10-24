// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "random_update.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <iterator>
#include <string>

#include "packet_pool.h"
#include "packet_reshape.h"

using bess::utils::be16_t;
using bess::utils::be32_t;

// "<name>/fields" holds the whole variable list; the class has no actions.
const Commands RandomUpdate::cmds = {};

const ResourceTables RandomUpdate::resource_tables = {
    ResourceTable::Singleton<bess::pb::RandomUpdateArg>("fields", true)};

CommandResponse RandomUpdate::Init(const bess::pb::RandomUpdateArg &arg) {
  // The init argument's variables; a refused one refuses the creation. The
  // same validation the resource uses: a list longer than vars_ is refused
  // instead of walking past the array.
  CommandResponse ret = ParseFields(arg, vars_, &num_vars_, &required_len_);
  if (ret.has_error()) {
    return ret;
  }
  arg_ = arg;

  // Stage converts the whole list; apply copies it in (no allocation).
  // ProcessBatch reads vars_ in place: paused workers only.
  auto registered = fields_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/fields",
      [](const bess::pb::RandomUpdateArg &v)
          -> std::expected<Staged, std::string> {
        Staged staged;
        CommandResponse r = ParseFields(v, staged.vars, &staged.num_vars,
                                        &staged.required_len);
        if (r.has_error()) {
          return std::unexpected(r.error().errmsg());
        }
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

void RandomUpdate::DeInit() {
  fields_resource_.Release();
}

CommandResponse RandomUpdate::ParseFields(const bess::pb::RandomUpdateArg &arg,
                                          Var *vars, size_t *num_vars,
                                          size_t *required_len) {
  if (static_cast<size_t>(arg.fields_size()) > kMaxVariable) {
    return CommandFailure(EINVAL, "max %zu variables can be specified",
                          kMaxVariable);
  }
  size_t required = 0;
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &vars[i]);
    if (ret.has_error()) {
      return ret;
    }
    // ParseField bounds offset + size by the data room: no overflow.
    required = std::max(required, vars[i].offset + vars[i].size);
  }
  *num_vars = arg.fields_size();
  *required_len = required;
  return CommandSuccess();
}

CommandResponse RandomUpdate::ParseField(
    const bess::pb::RandomUpdateArg::Field &var, Var *out) {
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
  return CommandSuccess();
}

// A packet that is too short in its first segment -- a runt, or a chain whose
// first segment ends before a field -- is dropped rather than partly written
// (D-172). A shared one is copied first (D-153); WritableHead checks the
// length again on the copy.
void RandomUpdate::PrepareBatch(Context *ctx, bess::PacketBatch *batch) {
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

// Writes a fresh value into every packet's `size`-byte field, big-endian.
template <size_t size, typename Var>
inline void WriteField(const Var &var, Random &rng, bess::PacketBatch *batch) {
  for (bess::PacketRef pkt : batch->packets()) {
    const uint32_t value = var.min + rng.GetRange(var.range);
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
}

}  // namespace

void RandomUpdate::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (num_vars_ != 0) {
    // Every field is written into the packet's first segment, so that
    // segment must hold the furthest byte any field names, and its payload
    // must be this packet's alone (D-153). One pass that only reads, and one
    // branch for the batch: a batch whose packets are all long enough and
    // unshared (the common case) is written as it is.
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

  Random &rng = rng_[ctx->wid];
  for (size_t i = 0; i < num_vars_; i++) {
    const Var &var = vars_[i];
    switch (var.size) {
      case 1:
        WriteField<1>(var, rng, batch);
        break;
      case 2:
        WriteField<2>(var, rng, batch);
        break;
      default:
        WriteField<4>(var, rng, batch);
        break;
    }
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(RandomUpdate, "rupdate", "updates packet data with random values")
