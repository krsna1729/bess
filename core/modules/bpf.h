// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_BPF_H_
#define BESS_MODULES_BPF_H_

#include <pcap.h>

#include <utility>
#include <vector>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/bpf.h"

class BPF final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  static const ResourceTables resource_tables;
  // The packet path only reads the compiled filter list.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  BPF() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::BPFArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  // Compiled filters this object owns: its destructor frees their code.
  struct OwnedFilters {
    std::vector<bess::utils::Filter> filters;

    OwnedFilters() = default;
    OwnedFilters(OwnedFilters &&other) noexcept
        : filters(std::move(other.filters)) {
      other.filters.clear();
    }
    OwnedFilters &operator=(OwnedFilters &&) = delete;
    ~OwnedFilters() { FreeFilters(&filters); }
  };

  // A "<name>/filters" value, compiled: the filters in priority order and
  // the BPFArg they came from (read back as given).
  struct Staged {
    OwnedFilters compiled;
    bess::pb::BPFArg arg;
    gate_idx_t default_gate = 0;
  };

  static bool Match(const bess::utils::Filter &, u_char *, u_int, u_int);
  // Compiles `f` into `*out` (the code is `*out`'s to free).
  static CommandResponse Compile(const bess::pb::BPFArg::Filter &f,
                                 bess::utils::Filter *out);
  // Frees the code of every filter of `*filters` and empties it.
  static void FreeFilters(std::vector<bess::utils::Filter> *filters);

  void ProcessBatch1Filter(Context *ctx, bess::PacketBatch *batch);
  gate_idx_t default_gate_ = 0;
  std::vector<bess::utils::Filter> filters_;
  // back and writes it whole.
  bess::pb::BPFArg arg_;
  // "<name>/filters" (D-129): the whole filter list; setting it replaces the
  // list. Paused workers: ProcessBatch reads filters_ in place.
  bess::framework::Singleton<bess::pb::BPFArg, Staged> filters_resource_;
};

#endif  // BESS_MODULES_BPF_H_
