// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_SEQUENTIALUPDATE_H_
#define BESS_MODULES_SEQUENTIALUPDATE_H_

#include "framework/singleton_resource.h"
#include "module.h"
#include "utils/endian.h"

#include "pb/supdate_msg.pb.h"

static const size_t kMaxVariable = 16;

class SequentialUpdate final : public Module {
public:
  static const Commands cmds;
  static const ResourceTables resource_tables;

  SequentialUpdate() : Module(), num_vars_(), vars_() {}

  CommandResponse Init(const sample::supdate::pb::SequentialUpdateArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse
  CommandAdd(const sample::supdate::pb::SequentialUpdateArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

private:
  struct Var {
    bess::utils::be32_t mask; // bits with 1 won't be updated
    uint32_t min;
    uint32_t range; // max - min + 1
    uint32_t cur;
    size_t offset;
    size_t bit_shift;
  };

  // A "<name>/fields" value, converted: the variables and the
  // SequentialUpdateArg they came from (read back as given).
  struct Staged {
    size_t num_vars = 0;
    Var vars[kMaxVariable] = {};
    sample::supdate::pb::SequentialUpdateArg arg;
  };

  // Validates and converts one field (its sequence starts at `min`).
  static CommandResponse
  ParseField(const sample::supdate::pb::SequentialUpdateArg::Field &var,
             Var *out);

  size_t num_vars_;

  Var vars_[kMaxVariable];

  // The fields as given, in order (control side): "<name>/fields" reads it
  // back.
  sample::supdate::pb::SequentialUpdateArg arg_;
  // "<name>/fields" (D-129): the whole field list; setting it replaces the
  // list and restarts every sequence. Paused workers: ProcessBatch reads and
  // writes vars_ in place.
  bess::framework::Singleton<sample::supdate::pb::SequentialUpdateArg, Staged>
      fields_resource_;
};

#endif // BESS_MODULES_SEQUENTIALUPDATE_H_
