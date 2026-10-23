// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "acl.h"

#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/udp.h"

// D-129: "<name>/rules" (a singleton) holds the whole rule list; the class
// has no actions.
const Commands ACL::cmds = {};

const ResourceTables ACL::resource_tables = {
    ResourceTable::Singleton<bess::pb::ACLArg>("rules", false).Rebuilds()};

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
  arg_ = arg;

  // Stage converts the whole list (a refused rule refuses the transaction);
  // apply swaps the published list and retires the old one a grace period
  // later, so workers keep running (no paused workers).
  auto registered = rules_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/rules",
      // clang-tidy 19's analyzer loses the unique_ptr moved into the returned
      // std::expected and reports a leak at the closing brace.
      // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
      [this](const bess::pb::ACLArg &v) -> std::expected<Staged, std::string> {
        std::vector<ACLRule> parsed;
        CommandResponse r = ParseRules(v, &parsed);
        if (r.has_error()) {
          return std::unexpected(r.error().errmsg());
        }
        return Staged{std::make_unique<const Rules>(Rules{std::move(parsed)}),
                      v};
      },
      // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
      [this](Staged &s, bess::dataplane::Retirer &retirer) {
        retirer.Retire(rules_.Exchange(std::move(s.rules)));
        arg_.Swap(&s.arg);
      },
      bess::dataplane::Resource::Footprint{.retires = 1},
      [this] { return arg_; }, nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void ACL::DeInit() {
  rules_resource_.Release();
}

void ACL::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Udp;

  gate_idx_t incoming_gate = ctx->current_igate;
  // One rule list for the whole batch.
  const std::vector<ACLRule> &rules = rules_.Read()->rules;

  for (bess::PacketRef pkt : batch->packets()) {
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
