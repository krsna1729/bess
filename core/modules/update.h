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
    be64_t value; /* in network order; the field's bytes are its first `size` */
    size_t offset;
    uint8_t size;
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

  // Validates and converts a whole field list. Init and "<name>/fields" share
  // it, so a list one refuses the other refuses too (Init used to index
  // fields_ without the limit check). On failure the array's contents are
  // unspecified and *num_fields is untouched.
  static CommandResponse ParseFields(const bess::pb::UpdateArg &arg,
                                     Field *fields, size_t *num_fields);

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
