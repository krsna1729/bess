// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ACL_H_
#define BESS_MODULES_ACL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
    // Every field is a wildcard (both prefixes /0 or "", both ports 0): the
    // rule names no IPv4 field, so it matches any frame, IPv4 or not.
    bool any;
  };

  // What the rules match, from the checked parse (core/packet_parse.h,
  // D-188): IPv4, untagged or behind up to two VLAN tags, options allowed.
  // The ports are TCP's or UDP's; ICMP, other protocols and a non-first
  // fragment (no transport header) have ports 0, which only a rule with
  // wildcard ports matches.
  struct Key {
    be32_t src;
    be32_t dst;
    be16_t src_port;
    be16_t dst_port;
  };

  // KeyOf's verdict on a frame.
  enum class Frame : uint8_t {
    kIpv4,       // `key` is filled: every rule can match
    kNotIpv4,    // a valid frame that is not IPv4 (ARP, IPv6, ...): only
                 // a rule with every field a wildcard matches
    kMalformed,  // headers that do not fit the frame or lie about their
                 // lengths: dropped before any rule
  };

  // The common frame is read at its fixed offsets after CheckFastIpv4, so it
  // decides as ParseFrame would. `frame` and `total_len` as ParseFrame's.
  static Frame KeyOf(std::span<const uint8_t> frame, size_t total_len, Key *key);

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
  // The rule list is one immutable object published through an RcuPtr: init
  // and the "<name>/rules" resource publish a replacement
  // while workers keep processing (G1.2 mode G; ACLs here are small,
  // scanned first-match). A batch reads it once. Decision D-017
  // (docs/decisions.md)
  struct Rules {
    std::vector<ACLRule> rules;
  };

  // A "<name>/rules" value, converted: the rule list to publish and the
  // ACLArg it came from (read back as given).
  struct Staged {
    std::unique_ptr<const Rules> rules;
    bess::pb::ACLArg arg;
  };

  // Validates and converts every rule of `arg`; nothing is kept on error.
  CommandResponse ParseRules(const bess::pb::ACLArg &arg,
                             std::vector<ACLRule> *out) const;
  void Install(std::vector<ACLRule> rules);

  bess::rcu::RcuPtr<Rules> rules_;
  // The rules as given, in order (control side): "<name>/rules" reads it
  // back.
  bess::pb::ACLArg arg_;
  // "<name>/rules" (D-129): the whole ordered rule list; setting it replaces
  // the list (an RCU swap: workers keep running).
  bess::framework::Singleton<bess::pb::ACLArg, Staged> rules_resource_;
};

#endif  // BESS_MODULES_ACL_H_
