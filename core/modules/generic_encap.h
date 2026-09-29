// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_GENERICENCAP_H_
#define BESS_MODULES_GENERICENCAP_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

#define MAX_FIELDS 8
#define MAX_FIELD_SIZE 8

struct Field {
  uint64_t value; /* onlt for constant values */
  int attr_id;    /* -1 for constant values */
  int pos;        /* relative position in the new header */
  int size;       /* in bytes. 1 <= size <= MAX_FIELD_SIZE */
};

class GenericEncap final : public Module {
 public:
  GenericEncap() : Module(), encap_size_(), num_fields_(), fields_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::GenericEncapArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  CommandResponse AddFieldOne(const bess::pb::GenericEncapArg_EncapField &field,
                              struct Field *f, int idx);

  int encap_size_;

  int num_fields_;

  struct Field fields_[MAX_FIELDS];
};

#endif  // BESS_MODULES_GENERICENCAP_H_
