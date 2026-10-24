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
  // Capabilities: the packet path advances each field's sequence, on one worker.
  static constexpr ModuleCapabilities kCapabilities{
      ModuleCapabilities::State::kSingleWorker, ModuleCapabilities::FlowAffinity::kNone};

  SequentialUpdate() : Module(), num_vars_(), required_len_(), vars_() {}

  CommandResponse Init(const sample::supdate::pb::SequentialUpdateArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // Appends the fields of `arg` to the list (Init's use: the creation
  // argument). The resource sets the whole list.
  CommandResponse AddFields(
      const sample::supdate::pb::SequentialUpdateArg &arg);

private:
  // One field: `size` bytes at `offset`, big-endian, written exactly (the
  // bytes around it are neither read nor written).
  struct Var {
    uint32_t min;
    uint32_t range; // max - min + 1
    uint32_t cur;
    size_t offset;
    uint8_t size; // 1, 2 or 4
  };

  // A "<name>/fields" value, converted: the variables, the first-segment
  // length they need, and the SequentialUpdateArg they came from (read back
  // as given).
  struct Staged {
    size_t num_vars = 0;
    size_t required_len = 0;
    Var vars[kMaxVariable] = {};
    sample::supdate::pb::SequentialUpdateArg arg;
  };

  // Validates and converts one field (its sequence starts at `min`).
  static CommandResponse
  ParseField(const sample::supdate::pb::SequentialUpdateArg::Field &var,
             Var *out);

  // The slow path for a batch with a packet that is not
  // HeadWritableInPlace (packet_reshape.h): drops what is too short, copies
  // what is shared, and compacts the batch.
  void PrepareBatch(Context *ctx, bess::PacketBatch *batch);

  size_t num_vars_;
  // The largest offset + size over vars_: a packet's first segment must hold
  // this many bytes for every field to be written (0 with no fields).
  size_t required_len_;

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
