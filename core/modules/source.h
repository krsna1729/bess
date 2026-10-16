// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_SOURCE_H_
#define BESS_MODULES_SOURCE_H_

#include <atomic>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

class Source final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;

  static const Commands cmds;
  // "settings" (SourceSettings): pkt_size and burst, applied live (D-129).
  static const ResourceTables resource_tables;

  Source() : Module(), pkt_size_(), burst_() { is_task_ = true; }

  CommandResponse Init(const bess::pb::SourceArg &arg);
  void DeInit() override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

 private:
  struct Settings {
    int pkt_size;
    int burst;
  };

  // Set by the settings resource while workers read it.
  std::atomic<int> pkt_size_;
  // Set by the settings resource while workers read it.
  std::atomic<int> burst_;

  // "<name>/settings"; after the state it sets.
  bess::framework::Singleton<bess::pb::SourceSettings, Settings> settings_;
};

#endif  // BESS_MODULES_SOURCE_H_