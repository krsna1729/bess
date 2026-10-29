// SPDX-License-Identifier: BSD-3-Clause

// Firewall's control side: rule parsing, the "<name>/rules" resource, Init,
// DeInit and PreResume. The packet path is in firewall.cc, a translation unit
// of its own: GCC's per-unit inline budget (--param inline-unit-growth) is
// shared by everything a unit instantiates, so control-side templates here
// cannot cost the batch loop an inlining decision.

#include "firewall.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <any>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "dataplane/resource_registry.h"
#include "framework/resource_codec.h"
#include "utils/endian.h"
#include "utils/logging.h"

// D-179: "<name>/rules" holds the policy, one entry per rule by id; the class
// has no actions.
const Commands Firewall::cmds = {};

const ResourceTables Firewall::resource_tables = {
    ResourceTable::Map<bess::pb::FirewallRuleKey, bess::pb::FirewallRuleValue>(
        "rules", /*requires_paused_workers=*/false)
        .Rebuilds()};

namespace {

using bess::classifier::FirewallAddr;
using bess::classifier::FirewallFamily;

// One prefix, as a rule names it: "a.b.c.d/len" or "x::y/len" (the length is
// required, so nothing is ambiguous).
struct Cidr {
  FirewallFamily family;
  FirewallAddr addr;
  uint8_t len;
};

std::optional<Cidr> ParseCidr(const std::string &text) {
  const size_t slash = text.find('/');
  if (slash == std::string::npos || slash + 1 >= text.size()) {
    return std::nullopt;
  }
  const std::string addr_text = text.substr(0, slash);
  const std::string len_text = text.substr(slash + 1);
  if (len_text.size() > 3 ||
      len_text.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const int len = std::stoi(len_text);

  Cidr out{};
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, addr_text.c_str(), &v4) == 1) {
    out.family = FirewallFamily::kIpv4;
    std::memcpy(out.addr.data(), &v4, 4);
    if (len > 32) {
      return std::nullopt;
    }
  } else if (inet_pton(AF_INET6, addr_text.c_str(), &v6) == 1) {
    out.family = FirewallFamily::kIpv6;
    std::memcpy(out.addr.data(), &v6, 16);
    if (len > 128) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  out.len = static_cast<uint8_t>(len);
  return out;
}

// One rule as the wire gives it, converted and checked. The refusal says what
// is wrong; the caller names the rule.
std::expected<bess::classifier::FirewallRule, std::string> ParseRule(
    const bess::pb::FirewallArg::Rule &rule) {
  // "" is a wildcard (documented): the field matches any address of the
  // rule's family.
  std::optional<Cidr> src;
  std::optional<Cidr> dst;
  if (!rule.src_ip().empty()) {
    src = ParseCidr(rule.src_ip());
    if (!src) {
      return std::unexpected("invalid IP prefix '" + rule.src_ip() + "'");
    }
  }
  if (!rule.dst_ip().empty()) {
    dst = ParseCidr(rule.dst_ip());
    if (!dst) {
      return std::unexpected("invalid IP prefix '" + rule.dst_ip() + "'");
    }
  }
  // The prefixes decide the rule's family. A rule with neither matches both
  // families; "any IPv4" is "0.0.0.0/0" and "any IPv6" is "::/0".
  FirewallFamily family = FirewallFamily::kAny;
  if (src && dst) {
    if (src->family != dst->family) {
      return std::unexpected("'" + rule.src_ip() + "' and '" + rule.dst_ip() +
                             "' are different families");
    }
    family = src->family;
  } else if (src) {
    family = src->family;
  } else if (dst) {
    family = dst->family;
  }

  if (rule.proto() > 0xff) {
    return std::unexpected("protocol " + std::to_string(rule.proto()) +
                           " is out of range");
  }
  // Ports are uint32 on the wire: check before narrowing, or 65616 would
  // quietly become port 80. An unset high bound is the full range, which is
  // why it is `optional` in the proto (a plain 0 default would mean "port
  // 0 only").
  const uint32_t src_high =
      rule.has_src_port_high() ? rule.src_port_high() : 0xffff;
  const uint32_t dst_high =
      rule.has_dst_port_high() ? rule.dst_port_high() : 0xffff;
  if (rule.src_port_low() > 0xffff || src_high > 0xffff ||
      rule.dst_port_low() > 0xffff || dst_high > 0xffff) {
    return std::unexpected("a port is out of range");
  }
  if (rule.drop() && rule.action_id() != 0) {
    return std::unexpected(
        "a rule that drops cannot name an action - the packet never reaches "
        "a module that would resolve it");
  }

  bess::classifier::FirewallRule parsed;
  parsed.family = family;
  if (src) {
    parsed.src_net = src->addr;
    parsed.src_len = src->len;
  }
  if (dst) {
    parsed.dst_net = dst->addr;
    parsed.dst_len = dst->len;
  }
  parsed.proto = static_cast<uint8_t>(rule.proto());
  parsed.src_port_low = static_cast<uint16_t>(rule.src_port_low());
  parsed.src_port_high = static_cast<uint16_t>(src_high);
  parsed.dst_port_low = static_cast<uint16_t>(rule.dst_port_low());
  parsed.dst_port_high = static_cast<uint16_t>(dst_high);
  parsed.action = rule.drop() ? Firewall::kDrop : Firewall::kPass;
  parsed.action_id = rule.action_id();
  // What the table itself refuses (a range the wrong way round), checked here
  // so a rule is refused as it arrives, never by the build that follows.
  if (std::optional<std::string> refused =
          bess::classifier::FirewallTable::Refusal(parsed)) {
    return std::unexpected(*refused);
  }
  return parsed;
}

}  // namespace

std::expected<std::unique_ptr<const Firewall::Published>, std::string>
Firewall::BuildPublished(const RuleView &rules) const {
  std::vector<Ranked> ranked;
  ranked.reserve(rules.size());
  for (const auto &r : rules) {
    ranked.push_back(Ranked{r.rule->priority, r.id, &r.rule->parsed});
  }
  // Ids are unique, so this order is total.
  std::sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) {
    return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
  });
  std::vector<bess::classifier::FirewallRule> parsed;
  parsed.reserve(ranked.size());
  for (const Ranked &r : ranked) {
    parsed.push_back(*r.rule);
  }
  // D-198: the table and the action ids it is published with, all glibc,
  // admitted before either is built; a refusal throws MemoryRefused and
  // refuses the Init or transaction with nothing built. The old table's
  // admission goes with it into the retirer at publish.
  const size_t bytes = bess::framework::MemoryCost<
                           bess::classifier::FirewallTable>::Bytes(parsed) +
                       ranked.size() * sizeof(uint32_t);
  bess::framework::MemoryLease lease =
      init_context().memory().Lease("rules", bytes, bytes);
  std::vector<uint32_t> action_ids;
  action_ids.reserve(ranked.size());
  for (const Ranked &r : ranked) {
    action_ids.push_back(r.rule->action_id);
  }
  auto table = bess::classifier::FirewallTable::Build(std::move(parsed));
  if (!table) {
    return std::unexpected(table.error());
  }
  return std::make_unique<const Published>(Published{
      std::move(lease), std::move(*table), std::move(action_ids)});
}

int Firewall::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Metadata offsets are assigned just before this. A write attribute has an
  // offset only when something reads it. A firewall nothing follows is a valid
  // pure filter, so the default name is quiet; a configured name promises a
  // reader, and its absence is a misconfiguration.
  if (!bess::metadata::IsValidOffset(attr_offset(action_id_attr_))) {
    if (!action_attr_configured_) {
      VLOG(1) << "Firewall '" << name() << "': nothing reads '"
              << action_attr_name_ << "'";
    } else {
      LOG(WARNING) << "Firewall '" << name() << "': nothing reads '"
                   << action_attr_name_
                   << "' (a module downstream with the same action_attr "
                      "would); packets carry no action id";
    }
  }
  return 0;
}

CommandResponse Firewall::Init(const bess::pb::FirewallArg &arg) {
  // D-141: the action a matching rule names goes to the packet's "action_id"
  // metadata, which a module downstream reads. The attribute has an offset
  // only when something reads it; without a reader there is nothing to write
  // and no action to resolve. A graph with two readers sets the name so each
  // reads its own.
  action_attr_configured_ = !arg.action_attr().empty();
  action_attr_name_ = arg.action_attr().empty() ? "action_id" : arg.action_attr();
  action_id_attr_ = AddMetadataAttr(action_attr_name_, sizeof(bess::utils::be32_t),
                                    bess::metadata::Attribute::AccessMode::kWrite);
  if (action_id_attr_ < 0) {
    return CommandFailure(-action_id_attr_, "add_metadata_attr() failed");
  }
  // The initial policy, in order: rule i is id i + 1 at priority 0, and equal
  // priorities go by id, so the given order is the policy order (D-179).
  RulesResource::Rules rules;
  for (int i = 0; i < arg.rules_size(); i++) {
    auto parsed = ParseRule(arg.rules(i));
    if (!parsed) {
      return CommandFailure(EINVAL, "rule %d: %s", i, parsed.error().c_str());
    }
    rules.emplace(static_cast<uint32_t>(i + 1),
                  KeyedRule{0, arg.rules(i), *parsed});
  }
  auto resource = std::make_unique<RulesResource>(
      name() + "/rules", published_, Builder{this}, "a firewall rule");
  if (auto initialized = resource->Initialize(std::move(rules)); !initialized) {
    return CommandFailure(EINVAL, "%s", initialized.error().c_str());
  }
  resource_ = std::move(resource);
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::FirewallRuleKey,
                                                   bess::pb::FirewallRuleValue>>(
          [](const bess::pb::FirewallRuleKey &key)
              -> std::expected<bess::dataplane::ResourceKey, std::string> {
            return bess::dataplane::EncodeKey(key.id());
          },
          [](const bess::pb::FirewallRuleValue &value)
              -> std::expected<std::any, std::string> {
            // An absent rule would decode as the empty rule - match all,
            // pass - and read back as a present one; a catch-all is written
            // as "rule": {}.
            if (!value.has_rule()) {
              return std::unexpected("a firewall rule value needs its rule");
            }
            auto parsed = ParseRule(value.rule());
            if (!parsed) {
              return std::unexpected(parsed.error());
            }
            return std::any(KeyedRule{value.priority(), value.rule(), *parsed});
          },
          // Read back as given: the id, the priority and the rule unchanged,
          // so a controller's desired state applies as a no-op (D-129).
          [this](const auto &visit) {
            bess::pb::FirewallRuleKey key;
            bess::pb::FirewallRuleValue value;
            for (const auto &[id, rule] : resource_->rules()) {
              key.set_id(id);
              value.set_priority(rule.priority);
              *value.mutable_rule() = rule.given;
              visit(key, value);
            }
          }));
  if (auto registered = init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Firewall::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // The rules may still be live: the module leaves with them.
  const std::string name = resource_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  resource_.reset();
}

ADD_MODULE(Firewall, "firewall",
           "a priority-ordered 4/6 5-tuple policy with port ranges (first match wins)")