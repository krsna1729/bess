// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_PORTINC_H_
#define BESS_MODULES_PORTINC_H_

#include <atomic>
#include <string>

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
  // The packet path only receives from the port's queues, counts, and writes
  // each packet's MARK when mark_attr is read; it keeps no state.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  PortInc() : Module(), port_(), prefetch_(), burst_() {
    is_task_ = true;
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::PortIncArg &arg);

  // The port it receives from; a task's argument is its queue id.
  const Port *port() const { return port_; }

  void DeInit() override;
  int OnEvent(bess::Event event) override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

  std::string GetDesc() const override;

 private:
  // mark_attr's offset when something reads it (D-185), else an invalid
  // offset: the packet path writes nothing.
  bess::metadata::mt_offset_t MarkOffset() const {
    return mark_attr_ < 0 ? bess::metadata::kMetadataOffsetNoWrite : attr_offset(mark_attr_);
  }

  Port *port_;
  int prefetch_;
  // Set by the settings resource while workers read it.
  std::atomic<int> burst_;
  // The configured MARK attribute (D-185); -1 when none.
  int mark_attr_ = -1;
  std::string mark_attr_name_;

  // "<name>/settings" (the burst); after the state it sets.
  bess::framework::Singleton<bess::pb::PortIncSettings, int> settings_;
};

#endif  // BESS_MODULES_PORTINC_H_
