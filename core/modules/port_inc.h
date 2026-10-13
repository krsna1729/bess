// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_PORTINC_H_
#define BESS_MODULES_PORTINC_H_

#include <atomic>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "port.h"

class PortInc final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;

  static const Commands cmds;
  // "settings" (PortIncSettings): burst, applied live (D-129).
  static const ResourceTables resource_tables;

  PortInc() : Module(), port_(), prefetch_(), burst_() {
    is_task_ = true;
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::PortIncArg &arg);

  // The port it receives from; a task's argument is its queue id.
  const Port *port() const { return port_; }
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &);

  void DeInit() override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

  std::string GetDesc() const override;

  CommandResponse CommandSetBurst(
      const bess::pb::PortIncCommandSetBurstArg &arg);

 private:
  Port *port_;
  int prefetch_;
  // Set by THREAD_SAFE commands and the settings resource while workers
  // read it.
  std::atomic<int> burst_;

  // "<name>/settings" (the burst); after the state it sets.
  bess::framework::Singleton<bess::pb::PortIncSettings, int> settings_;
};

#endif  // BESS_MODULES_PORTINC_H_
