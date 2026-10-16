// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_RANDOMUPDATE_H_
#define BESS_MODULES_RANDOMUPDATE_H_

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

#include "utils/endian.h"
#include "utils/random.h"

static const size_t kMaxVariable = 16;

class RandomUpdate final : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;

  RandomUpdate() : Module(), num_vars_(), vars_(), rng_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::RandomUpdateArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  struct Var {
    bess::utils::be32_t mask;  // bits with 1 won't be updated
    uint32_t min;
    uint32_t range;  // max - min + 1
    size_t offset;
    size_t bit_shift;
  };

  // A "<name>/fields" value, converted: the variables and the
  // RandomUpdateArg they came from (read back as given).
  struct Staged {
    size_t num_vars = 0;
    Var vars[kMaxVariable] = {};
    bess::pb::RandomUpdateArg arg;
  };

  // Validates and converts one field.
  static CommandResponse ParseField(const bess::pb::RandomUpdateArg::Field &var,
                                    Var *out);

  size_t num_vars_;

  Var vars_[kMaxVariable];

  Random rng_;

  // The fields as given, in order (control side): "<name>/fields" reads it
  // back.
  bess::pb::RandomUpdateArg arg_;
  // "<name>/fields" (D-129): the whole field list; setting it replaces the
  // list. Paused workers: ProcessBatch reads vars_ in place.
  bess::framework::Singleton<bess::pb::RandomUpdateArg, Staged> fields_resource_;
};

#endif  // BESS_MODULES_RANDOMUPDATE_H_
