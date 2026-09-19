// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_MODULES_IPLOOKUP_H_
#define BESS_MODULES_IPLOOKUP_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/endian.h"

using bess::utils::be32_t;
using ParsedPrefix = std::tuple<int, std::string, be32_t>;

class IPLookup final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  IPLookup() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::IPLookupArg &arg);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::IPLookupCommandAddArg &arg);
  CommandResponse CommandDelete(const bess::pb::IPLookupCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

 private:
  // One rule as the control plane last set it. This list, not rte_lpm, is the
  // source of truth a rebuild is made from -- rte_lpm cannot be read back.
  struct Route {
    be32_t prefix;
    uint8_t prefix_len;
    gate_idx_t gate;
  };

  // One immutable routing generation. ProcessBatch() takes a snapshot of the
  // current generation once per batch and never observes a *mutated* table:
  // each routing command builds a replacement off the data path and publishes
  // it atomically, and a retired generation is freed by shared_ptr as soon as
  // the last batch holding it returns. That is what lets route updates run
  // while workers keep forwarding, instead of requiring workers to be paused
  // (MODERNIZATION.md entry 35).
  struct Generation {
    ~Generation();

    std::vector<Route> routes;
    struct rte_lpm *lpm = nullptr;
    gate_idx_t default_gate = DROP_GATE;
  };

  using GenerationPtr = std::shared_ptr<const Generation>;

  // Builds a generation from `routes`, or returns nullptr with `*err` set to
  // the errno a caller can report. Called with `mutation_lock_` held, or
  // before the module is running.
  GenerationPtr Build(const std::vector<Route> &routes, gate_idx_t default_gate,
                      int *err);

  // Publishes `next` and then waits for the readers of `current` to drain, so
  // the retired generation is freed on this (control-plane) thread rather than
  // on a packet worker. `current` must be the caller's only reference to the
  // generation (the wait is until its use count drops to that one); caller
  // holds mutation_lock_ too.
  void Publish(GenerationPtr next, const GenerationPtr &current);

  ParsedPrefix ParseIpv4Prefix(const std::string &prefix, uint64_t prefix_len);

  // nullptr only before Init() and after DeInit().
  std::atomic<GenerationPtr> table_;
  std::mutex mutation_lock_;  // serializes routing-command rebuilds
  uint32_t max_rules_ = 0;    // from Init(), reused for every rebuild
  uint32_t max_tbl8s_ = 0;
};

#endif  // BESS_MODULES_IPLOOKUP_H_
