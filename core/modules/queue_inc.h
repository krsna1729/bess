// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_QUEUEINC_H_
#define BESS_MODULES_QUEUEINC_H_

#include <atomic>

#include "module.h"
#include "pb/module_msg.pb.h"
#include "port.h"

class QueueInc final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;

  static const Commands cmds;

  QueueInc() : Module(), port_(), qid_(), prefetch_(), burst_() {}

  CommandResponse Init(const bess::pb::QueueIncArg &arg);

  const Port *port() const { return port_; }
  queue_t qid() const { return qid_; }
  void DeInit() override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

  std::string GetDesc() const override;

  CommandResponse CommandSetBurst(
      const bess::pb::QueueIncCommandSetBurstArg &arg);

 private:
  Port *port_;
  queue_t qid_;
  int prefetch_;
  // Set by THREAD_SAFE commands while workers read it.
  std::atomic<int> burst_;
};

#endif  // BESS_MODULES_QUEUEINC_H_
