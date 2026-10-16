// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_DUMP_H_
#define BESS_MODULES_DUMP_H_

#include <atomic>
#include <cstdint>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

class Dump final : public Module {
 public:
  CommandResponse Init(const bess::pb::DumpArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  static const Commands cmds;
  static const ResourceTables resource_tables;

 private:
  // The init argument's interval, validated before anything is installed.
  CommandResponse SetInterval(const bess::pb::DumpArg &arg);

  // Workers read it while it is set (relaxed): "<name>/settings" applies
  // while they run.
  std::atomic<uint64_t> min_interval_ns_;
  // The interval as last set, in seconds (control side, the read-back).
  double interval_sec_ = 1.0;
  uint64_t next_ns_;

  bess::framework::Singleton<bess::pb::DumpArg, double> settings_;
};

#endif  // BESS_MODULES_DUMP_H_
