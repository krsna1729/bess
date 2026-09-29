// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_BPF_H_
#define BESS_MODULES_BPF_H_

#include <pcap.h>

#include <vector>

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/bpf.h"

class BPF final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  BPF() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::BPFArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse GetInitialArg(const bess::pb::EmptyArg &);
  CommandResponse CommandAdd(const bess::pb::BPFArg &arg);
  CommandResponse CommandDelete(const bess::pb::BPFArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

 private:
  static bool Match(const bess::utils::Filter &, u_char *, u_int, u_int);

  void ProcessBatch1Filter(Context *ctx, bess::PacketBatch *batch);

  std::vector<bess::utils::Filter> filters_;
};

#endif  // BESS_MODULES_BPF_H_
