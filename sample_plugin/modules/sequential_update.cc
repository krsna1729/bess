// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "sequential_update.h"

#include <algorithm>
#include <expected>
#include <iterator>
#include <string>

#include "framework/plugin.h"

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
        }
        staged.num_vars = v.fields_size();
        staged.arg = v;
        return staged;
      },
      [this](Staged &s) {
        std::copy(std::begin(s.vars), std::end(s.vars), std::begin(vars_));
        num_vars_ = s.num_vars;
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
  size_t size;
  size_t offset;
  be32_t mask;
  uint32_t min;
  uint32_t max;

  offset = var.offset();
  size = var.size();
  min = var.min();
  max = var.max();

  switch (size) {
  case 1:
    mask = be32_t(0x00ffffff);
    min = std::min(min, static_cast<uint32_t>(0xff));
    max = std::min(max, static_cast<uint32_t>(0xff));
    break;

  case 2:
    mask = be32_t(0x0000ffff);
    min = std::min(min, static_cast<uint32_t>(0xffff));
    max = std::min(max, static_cast<uint32_t>(0xffff));
    break;

  case 4:
    mask = be32_t(0x00000000);
    min = std::min(min, static_cast<uint32_t>(0xffffffffu));
    max = std::min(max, static_cast<uint32_t>(0xffffffffu));
    break;

  default:
    return CommandFailure(EINVAL, "'size' must be 1, 2, or 4");
  }

  if (offset + size > SNBUF_DATA) {
    return CommandFailure(EINVAL, "too large 'offset'");
  }

  if (min > max) {
    return CommandFailure(EINVAL, "'min' should not be greater than 'max'");
  }

  out->offset = offset;
  out->mask = mask;
  out->min = min;

  // avoid modulo 0
  out->range = (max - min + 1) ?: 0xffffffff;
  out->cur = 0;
  out->bit_shift = (4 - size) * 8;
  return CommandSuccess();
}

CommandResponse SequentialUpdate::AddFields(
    const sample::supdate::pb::SequentialUpdateArg &arg) {
  size_t curr = num_vars_;
  if (curr + arg.fields_size() > kMaxVariable) {
    return CommandFailure(EINVAL, "max %zu variables can be specified",
                          kMaxVariable);
  }

  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &vars_[curr + i]);
    if (ret.has_error()) {
      return ret;
    }
  }

  num_vars_ = curr + arg.fields_size();
  arg_.mutable_fields()->MergeFrom(arg.fields());
  return CommandSuccess();
}

void SequentialUpdate::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  for (size_t i = 0; i < num_vars_; i++) {
    const auto var = &vars_[i];

    be32_t mask = var->mask;
    uint32_t min = var->min;
    uint32_t range = var->range;
    uint32_t cur = var->cur;
    size_t offset = var->offset;
    size_t bit_shift = var->bit_shift;

    for (int j = 0; j < cnt; j++) {
      be32_t *p = batch->packet(j).head_data<be32_t *>(offset);
      uint32_t value = min + cur;
      cur = cur + 1;
      if (cur >= range) {
        cur = 0;
      }

      *p = (*p & mask) | (be32_t(value) << bit_shift);
    }

    var->cur = cur;
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(SequentialUpdate, "supdate",
           "updates packet data sequentially in a range")

BESS_PLUGIN_REQUIRES("sample_sequential_update", "1.0.0",
                     BESS_CAP_INIT_CONTEXT | BESS_CAP_RESOURCES |
                         BESS_CAP_EVENTS | BESS_CAP_EVENT_SOURCES |
                         BESS_CAP_TICKS)
