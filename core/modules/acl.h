// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ACL_H_
#define BESS_MODULES_ACL_H_

#include <vector>

#include "runtime/runtime_state.h"
#include "../module.h"
#include "../rcu/rcu_ptr.h"
#include "../pb/module_msg.pb.h"
#include "../utils/ip.h"

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::Ipv4Prefix;

class ACL final : public Module {
 public:
  struct ACLRule {
    bool Match(be32_t sip, be32_t dip, be16_t sport, be16_t dport) const {
      return src_ip.Match(sip) && dst_ip.Match(dip) &&
             (src_port == be16_t(0) || src_port == sport) &&
             (dst_port == be16_t(0) || dst_port == dport);
    }

    Ipv4Prefix src_ip;
    Ipv4Prefix dst_ip;
    be16_t src_port;
    be16_t dst_port;
    bool drop;
  };

  static const Commands cmds;

  ACL() : Module(), rules_(bess::runtime::runtime().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::ACLArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::ACLArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

 private:
  // The rule list is one immutable object published through an RcuPtr:
  // `add` and `clear` publish a replacement while workers keep processing
  // (G1.2 mode G; ACLs here are small, scanned first-match). A batch reads it
  // once. Decision D-017 (docs/decisions.md)
  struct Rules {
    std::vector<ACLRule> rules;
  };

  // Validates and converts every rule of `arg`; nothing is kept on error.
  CommandResponse ParseRules(const bess::pb::ACLArg &arg,
                             std::vector<ACLRule> *out) const;
  void Install(std::vector<ACLRule> rules);

  bess::rcu::RcuPtr<Rules> rules_;
};

#endif  // BESS_MODULES_ACL_H_
