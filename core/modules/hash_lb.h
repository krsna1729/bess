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

#ifndef BESS_MODULES_HASHLB_H_
#define BESS_MODULES_HASHLB_H_

#include <memory>
#include <vector>

#include "../control/runtime_state.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../rcu/rcu_ptr.h"
#include "../utils/exact_match_table.h"

using bess::utils::ExactMatchField;
using bess::utils::ExactMatchKey;
using bess::utils::ExactMatchKeyHash;
using bess::utils::ExactMatchTable;

// Splits packets across output gates by a hash of L2/L3/L4 or chosen fields.
//
// The configuration (mode, gate list, field layout) is one immutable object
// published through an RcuPtr: `set_mode` and `set_gates` build a replacement
// and publish it while workers keep processing (G1.2 mode G; the
// configuration is small and changes rarely). A batch reads it once.
// Decision D-017 (docs/decisions.md)
class HashLB final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  HashLB() : Module(), config_(bess::control::runtime().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::HashLBArg &arg);

  std::string GetDesc() const override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandSetMode(const bess::pb::HashLBCommandSetModeArg &arg);
  CommandResponse CommandSetGates(
      const bess::pb::HashLBCommandSetGatesArg &arg);

 private:
  enum class Mode { kL2, kL3, kL4, kOther };
  static constexpr Mode kDefaultMode = Mode::kL4;
  static constexpr size_t kMaxGates = 16384;

  struct Config {
    Mode mode = kDefaultMode;
    std::vector<gate_idx_t> gates;
    // Built once per set_mode and never changed, so configurations share it.
    // No rules are ever added; it is only used for MakeKeys().
    std::shared_ptr<const ExactMatchTable<int>> fields_table =
        std::make_shared<const ExactMatchTable<int>>();
    ExactMatchKeyHash hasher{0};
  };

  template <Mode mode>
  inline void DoProcessBatch(Context *ctx, bess::PacketBatch *batch,
                             const Config &config);

  // A copy of the current configuration (or a default one before Init).
  Config Current() const;
  // Validates and applies a mode/field change to `config`.
  CommandResponse ApplyMode(const bess::pb::HashLBCommandSetModeArg &arg,
                            Config *config) const;
  // Validates every wire gate before narrowing it, then replaces the list.
  CommandResponse ApplyGates(const bess::pb::HashLBCommandSetGatesArg &arg,
                             Config *config) const;
  void Install(Config config);

  bess::rcu::RcuPtr<Config> config_;
};

#endif  // BESS_MODULES_HASHLB_H_
