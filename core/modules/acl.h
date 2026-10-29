// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ACL_H_
#define BESS_MODULES_ACL_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "framework/memory_scope.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "rcu/rcu_ptr.h"
#include "pb/module_msg.pb.h"
#include "utils/ip.h"

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::Ipv4Prefix;

class ACL final : public Module {
 public:
  // A rule as given: a "" prefix or port 0 is a wildcard.
  struct ACLRule {
    Ipv4Prefix src_ip;
    Ipv4Prefix dst_ip;
    be16_t src_port;
    be16_t dst_port;
    bool drop;
  };

  // What a rule tests on a frame: its IPv4 source and destination and its
  // TCP or UDP source and destination ports, each as its bytes on the wire
  // (`addrs` is bytes 12-19 of the IPv4 header, `ports` bytes 0-3 of the
  // transport header). From the checked parse (core/packet_parse.h, D-188):
  // IPv4, untagged or behind up to two VLAN tags, options allowed. ICMP,
  // other protocols and a non-first fragment (no transport header) have ports
  // 0, which only a rule with wildcard ports matches.
  struct Key {
    uint64_t addrs;
    uint32_t ports;
  };

  // A rule as the packet path tests it: the key's bits it names (a wildcard
  // field's mask is 0) and the values they must have. A rule constrains only
  // the fields it names.
  struct MatchRule {
    bool Match(const Key &key) const {
      return (key.addrs & addr_mask) == addr_want && (key.ports & port_mask) == port_want;
    }

    uint64_t addr_mask;
    uint64_t addr_want;
    uint32_t port_mask;
    uint32_t port_want;
    bool pass;
  };

  // The rule list the packet path scans, from the rules as given (in order).
  // A rule with every field a wildcard matches every frame, IPv4 or not, so
  // the first such rule decides every frame no earlier rule matched, and
  // the rules after it are never reached: `named` holds the rules before it,
  // `otherwise_pass` its verdict (drop when there is none). A frame that is
  // not IPv4 matches no rule of `named` (each names an IPv4 field).
  struct Rules {
    static Rules Compile(const std::vector<ACLRule> &rules);
    // Retained with the compiled rules until their RCU grace period ends.
    bess::framework::MemoryLease lease;

    std::vector<MatchRule> named;
    bool otherwise_pass = false;
  };

  static const Commands cmds;
  static const ResourceTables resource_tables;
  // The packet path only reads the RCU-published rule list.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  ACL() : Module(), rules_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::ACLArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  // A "<name>/rules" value, converted: the rule list to publish and the
  // ACLArg it came from (read back as given).
  struct Staged {
    std::unique_ptr<const Rules> rules;
    bess::pb::ACLArg arg;
  };

  // Validates every rule, then admits and compiles the packet-path list.
  CommandResponse ParseRules(const bess::pb::ACLArg &arg, Rules *out) const;
  void Install(Rules rules);

  // The rule list is one immutable object published through an RcuPtr: init
  // and the "<name>/rules" resource publish a replacement
  // while workers keep processing (G1.2 mode G; ACLs here are small,
  // scanned first-match). A batch reads it once. Decision D-017
  // (docs/decisions.md)
  bess::rcu::RcuPtr<Rules> rules_;
  // The rules as given, in order (control side): "<name>/rules" reads it
  // back.
  bess::pb::ACLArg arg_;
  // "<name>/rules" (D-129): the whole ordered rule list; setting it replaces
  // the list (an RCU swap: workers keep running).
  bess::framework::Singleton<bess::pb::ACLArg, Staged> rules_resource_;
};

#endif  // BESS_MODULES_ACL_H_
