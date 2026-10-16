// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_UPDATE_H_
#define BESS_MODULES_UPDATE_H_

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

#include "utils/endian.h"

using bess::utils::be64_t;

class Update final : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;

  Update() : Module(), num_fields_(), fields_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::UpdateArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  static const size_t kMaxFields = 16;

  struct Field {
    be64_t mask;  /* bits with 1 won't be updated */
    be64_t value; /* in network order */
    size_t offset;
  };

  // A "<name>/fields" value, converted: the fields and the UpdateArg they
  // came from (read back as given).
  struct Staged {
    size_t num_fields = 0;
    Field fields[kMaxFields] = {};
    bess::pb::UpdateArg arg;
  };

  // Validates and converts one field.
  static CommandResponse ParseField(const bess::pb::UpdateArg::Field &field,
                                    Field *out);

  size_t num_fields_;

  Field fields_[kMaxFields];

  // The fields as given, in order (control side): "<name>/fields" reads it
  // back.
  bess::pb::UpdateArg arg_;
  // "<name>/fields" (D-129): the whole field list; setting it replaces the
  // list. Paused workers: ProcessBatch reads fields_ in place.
  bess::framework::Singleton<bess::pb::UpdateArg, Staged> fields_resource_;
};

#endif  // BESS_MODULES_UPDATE_H_
