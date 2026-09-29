// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_PORTOUT_H_
#define BESS_MODULES_PORTOUT_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../port.h"
#include "../packet_tx_checksum.h"
#include "../utils/mcslock.h"
#include "../worker.h"

class PortOut final : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = 0;

  static const Commands cmds;

  PortOut()
      : Module(),
        port_(),
        tx_checksum_profile_(),
        tx_checksum_profile_arg_(),
        has_tx_checksum_profile_(false),
        worker_queues_(),
        queue_users_(),
        queue_locks_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::PortOutArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  int OnEvent(bess::Event e) override;

  std::string GetDesc() const override;

 private:
  Port *port_;

  bess::packet::BoundTxFinalizationProfile tx_checksum_profile_;
  bess::pb::TxChecksumProfile tx_checksum_profile_arg_;
  bool has_tx_checksum_profile_;

  int worker_queues_[Worker::kMaxWorkers];

  // Number of workers mapped to a given queue. Indexed by queue number
  int queue_users_[MAX_QUEUES_PER_DIR];

  mcslock_t queue_locks_[MAX_QUEUES_PER_DIR];
};

#endif  // BESS_MODULES_PORTOUT_H_
