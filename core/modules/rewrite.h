// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_REWRITE_H_
#define BESS_MODULES_REWRITE_H_

#include <memory>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "stats/worker_local.h"

class Rewrite final : public Module {
 public:
  static const size_t kNumSlots = bess::PacketBatch::kMaxBurst * 2 - 1;
  static const size_t kMaxTemplateSize = 1536;

  static const Commands cmds;
  static const ResourceTables resource_tables;
  // Each worker advances its own round robin; packets need no flow affinity.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kPerWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  Rewrite()
      : Module(),
        next_turn_(),
        templates_(std::make_unique<Templates>()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::RewriteArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  // The templates, replicated round robin over kNumSlots slots so that a
  // batch starting at any template reads consecutive slots.
  struct Templates {
    size_t num = 0;
    // precalculated "index % num": the next turn after slot `index`
    // (index up to kNumSlots: a full batch from the last template)
    size_t jump[kNumSlots + 1] = {};
    uint16_t size[kNumSlots] = {};
    unsigned char data[kNumSlots][kMaxTemplateSize] = {};
  };

  // A "<name>/templates" value, built: the templates and the RewriteArg they
  // came from (read back as given).
  struct Staged {
    std::unique_ptr<Templates> templates;
    bess::pb::RewriteArg arg;
  };

  // Builds `arg`'s templates into `*out` (fresh); refuses what `add`
  // refuses.
  static CommandResponse Build(const bess::pb::RewriteArg &arg,
                               Templates *out);

  inline void DoRewrite(const Templates &t, bess::PacketBatch *batch, size_t &next_turn);
  inline void DoRewriteSingle(const Templates &t, bess::PacketBatch *batch);

  // Each worker remembers its next index in [0, num), on separate cache lines.
  bess::stats::WorkerLocal<size_t> next_turn_;
  std::unique_ptr<Templates> templates_;  // never null

  // The templates as given, in order (control side): "<name>/templates"
  // reads it back.
  bess::pb::RewriteArg arg_;
  // "<name>/templates" (D-129): the whole template list; setting it
  // replaces the list. Paused workers: ProcessBatch reads templates_ and
  // next_turn_ in place.
  bess::framework::Singleton<bess::pb::RewriteArg, Staged> templates_resource_;
};

#endif  // BESS_MODULES_REWRITE_H_
