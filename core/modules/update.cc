// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "update.h"

#include <algorithm>
#include <expected>
#include <iterator>
#include <string>

#include "utils/endian.h"

// "<name>/fields" holds the whole field list; the class has no actions.
const Commands Update::cmds = {};

const ResourceTables Update::resource_tables = {
    ResourceTable::Singleton<bess::pb::UpdateArg>("fields", true)};

CommandResponse Update::Init(const bess::pb::UpdateArg &arg) {
  // The init argument's fields; a refused one refuses the creation.
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse ret = ParseField(arg.fields(i), &fields_[i]);
    if (ret.has_error()) {
      return ret;
    }
  }
  num_fields_ = arg.fields_size();
  arg_ = arg;

  // Stage converts the whole list; apply copies it in (no allocation).
  // ProcessBatch reads fields_ in place: paused workers only.
  auto registered = fields_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/fields",
      [](const bess::pb::UpdateArg &v) -> std::expected<Staged, std::string> {
        if (static_cast<size_t>(v.fields_size()) > kMaxFields) {
          return std::unexpected("max " + std::to_string(kMaxFields) +
                                 " variables can be specified");
        }
        Staged staged;
        for (int i = 0; i < v.fields_size(); i++) {
          CommandResponse r = ParseField(v.fields(i), &staged.fields[i]);
          if (r.has_error()) {
            return std::unexpected(r.error().errmsg());
          }
        }
        staged.num_fields = v.fields_size();
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
    const auto field = &fields_[i];

    be64_t mask = field->mask;
    be64_t value = field->value;
    int16_t offset = field->offset;  // could be < 0

    for (bess::PacketRef snb : batch->packets()) {
      char *head = snb.head_data<char *>();

      be64_t *p = reinterpret_cast<be64_t *>(head + offset);
      *p = (*p & mask) | value;
    }
  }

  RunNextModule(ctx, batch);
}

CommandResponse Update::ParseField(const bess::pb::UpdateArg::Field &field,
                                   Field *out) {
  size_t size = field.size();
  if (size < 1 || size > 8) {
    return CommandFailure(EINVAL, "'size' must be 1-8");
  }

  if (field.offset() + size > SNBUF_DATA) {
    return CommandFailure(EINVAL, "too large 'offset'");
  }

  be64_t value(field.value() << ((8 - size) * 8));
  be64_t mask((1ull << ((8 - size) * 8)) - 1);

  if ((value & mask).value() != 0) {
    LOG(INFO) << value << ' ' << mask;
    return CommandFailure(
        EINVAL, "'value' field has not a correct %zu-byte value", size);
  }

  out->offset = field.offset();
  out->mask = mask;
  out->value = value;
  return CommandSuccess();
}

ADD_MODULE(Update, "update", "updates packet data with specified values")
