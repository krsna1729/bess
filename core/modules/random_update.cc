// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "random_update.h"

#include <algorithm>
#include <expected>
#include <iterator>
#include <string>

using bess::utils::be32_t;

// "<name>/fields" holds the whole variable list; the class has no actions.
const Commands RandomUpdate::cmds = {};

const ResourceTables RandomUpdate::resource_tables = {
    ResourceTable::Singleton<bess::pb::RandomUpdateArg>("fields", true)};

CommandResponse RandomUpdate::Init(const bess::pb::RandomUpdateArg &arg) {
  // The init argument's variables; a refused one refuses the creation.
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &vars_[i]);
    if (ret.has_error()) {
      return ret;
    }
  }
  num_vars_ = arg.fields_size();
  arg_ = arg;

  // Stage converts the whole list; apply copies it in (no allocation).
  // ProcessBatch reads vars_ in place: paused workers only.
  auto registered = fields_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/fields",
      [](const bess::pb::RandomUpdateArg &v)
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

void RandomUpdate::DeInit() {
  fields_resource_.Release();
}

CommandResponse RandomUpdate::ParseField(
    const bess::pb::RandomUpdateArg::Field &var, Var *out) {
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
  out->bit_shift = (4 - size) * 8;
  return CommandSuccess();
}

void RandomUpdate::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  Random &rng = rng_[ctx->wid];
  for (size_t i = 0; i < num_vars_; i++) {
    const auto var = &vars_[i];

    be32_t mask = var->mask;
    uint32_t min = var->min;
    uint32_t range = var->range;
    size_t offset = var->offset;
    size_t bit_shift = var->bit_shift;

    for (bess::PacketRef pkt : batch->packets()) {
      be32_t *p = pkt.head_data<be32_t *>(offset);
      uint32_t rand_val = min + rng.GetRange(range);
      *p = (*p & mask) | (be32_t(rand_val) << bit_shift);
    }
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(RandomUpdate, "rupdate", "updates packet data with random values")
