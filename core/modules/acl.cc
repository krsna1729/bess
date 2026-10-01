// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "acl.h"

#include <optional>
#include <string>

#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/udp.h"

const Commands ACL::cmds = {
    {"add", "ACLArg", MODULE_CMD_FUNC(&ACL::CommandAdd),
     Command::THREAD_SAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&ACL::CommandClear),
     Command::THREAD_SAFE}};

CommandResponse ACL::ParseRules(const bess::pb::ACLArg &arg,
                                std::vector<ACLRule> *out) const {
  out->clear();
  int i = 0;
  for (const auto &rule : arg.rules()) {
    // Ports are uint32 on the wire: check before narrowing, or 65616 would
    // quietly become port 80.
    if (rule.src_port() > 0xffff || rule.dst_port() > 0xffff) {
      return CommandFailure(EINVAL, "rule %d: port out of range", i);
    }
    // "" is a wildcard (documented); anything else must be "a.b.c.d/len".
    const auto parse = [](const std::string &text)
        -> std::optional<Ipv4Prefix> {
      return text.empty() ? std::optional<Ipv4Prefix>(Ipv4Prefix(""))
                          : Ipv4Prefix::Parse(text);
    };
    const auto src = parse(rule.src_ip());
    const auto dst = parse(rule.dst_ip());
    if (!src || !dst) {
      return CommandFailure(EINVAL, "rule %d: invalid IP prefix '%s'", i,
                            (!src ? rule.src_ip() : rule.dst_ip()).c_str());
    }
    out->push_back({.src_ip = *src,
                    .dst_ip = *dst,
                    .src_port = be16_t(static_cast<uint16_t>(rule.src_port())),
                    .dst_port = be16_t(static_cast<uint16_t>(rule.dst_port())),
                    .drop = rule.drop()});
    i++;
  }
  return CommandSuccess();
}

void ACL::Install(std::vector<ACLRule> rules) {
  auto next = std::make_unique<const Rules>(Rules{std::move(rules)});
  if (rules_.Read() == nullptr) {
    rules_.Initialize(std::move(next));
    return;
  }
  rules_.Publish(std::move(next));
  init_context().rcu().ReclaimReady();
}

CommandResponse ACL::Init(const bess::pb::ACLArg &arg) {
  std::vector<ACLRule> rules;
  CommandResponse ret = ParseRules(arg, &rules);
  if (ret.has_error()) {
    return ret;
  }
  Install(std::move(rules));
  return CommandSuccess();
}

// All or nothing: a rule that fails validation leaves the ACL unchanged.
CommandResponse ACL::CommandAdd(const bess::pb::ACLArg &arg) {
  std::vector<ACLRule> added;
  CommandResponse ret = ParseRules(arg, &added);
  if (ret.has_error()) {
    return ret;
  }
  std::vector<ACLRule> rules = rules_.Read()->rules;
  rules.insert(rules.end(), added.begin(), added.end());
  Install(std::move(rules));
  return CommandSuccess();
}

CommandResponse ACL::CommandClear(const bess::pb::EmptyArg &) {
  Install({});
  return CommandSuccess();
}

void ACL::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Udp;

  gate_idx_t incoming_gate = ctx->current_igate;
  // One rule list for the whole batch.
  const std::vector<ACLRule> &rules = rules_.Read()->rules;

  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);

    Ethernet *eth = pkt.head_data<Ethernet *>();
    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);
    size_t ip_bytes = ip->header_length << 2;
    Udp *udp =
        reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);

    bool emitted = false;
    for (const auto &rule : rules) {
      if (rule.Match(ip->src, ip->dst, udp->src_port, udp->dst_port)) {
        if (!rule.drop) {
          emitted = true;
          EmitPacket(ctx, pkt, incoming_gate);
        }
        break;  // Stop matching other rules
      }
    }

    if (!emitted) {
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(ACL, "acl", "ACL module from NetBricks")
