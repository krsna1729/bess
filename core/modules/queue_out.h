// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_QUEUEOUT_H_
#define BESS_MODULES_QUEUEOUT_H_

#include "module.h"
#include "packet_tx_checksum.h"
#include "pb/module_msg.pb.h"
#include "port.h"

class QueueOut final : public Module {
 public:
  static const gate_idx_t kNumOGates = 0;

  QueueOut()
      : Module(), port_(), qid_(), tx_checksum_profile_() {}

  CommandResponse Init(const bess::pb::QueueOutArg &arg);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

 private:
  Port *port_;
  queue_t qid_;
  bess::packet::BoundTxFinalizationProfile tx_checksum_profile_;
};

#endif  // BESS_MODULES_QUEUEOUT_H_
