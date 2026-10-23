// SPDX-License-Identifier: BSD-3-Clause

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
#include "packet_parse.h"
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
Firewall::BuildPublished(std::vector<Ranked> rules) {
  // Ids are unique, so this order is total.
  std::sort(rules.begin(), rules.end(), [](const Ranked &a, const Ranked &b) {
    return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
  });
  std::vector<bess::classifier::FirewallRule> parsed;
  std::vector<uint32_t> action_ids;
  parsed.reserve(rules.size());
  action_ids.reserve(rules.size());
  for (const Ranked &r : rules) {
    parsed.push_back(*r.rule);
    action_ids.push_back(r.rule->action_id);
  }
  auto table = bess::classifier::FirewallTable::Build(std::move(parsed));
  if (!table) {
    return std::unexpected(table.error());
  }
  return std::make_unique<const Published>(
      Published{std::move(*table), std::move(action_ids)});
}

// "<module>/rules" (D-179): key EncodeKey(the rule id), value a KeyedRule the
// codec has already checked. The policy is rebuilt off the packet path once
// per transaction: Reserve() records each operation (an upsert's map node is
// allocated there), Prepare() builds the one table the committed rules and
// the operations make together, and the transaction's first Publish() swaps
// it in whole - one RCU exchange, one retire - then moves the nodes into the
// committed rules without allocating. The workers keep running and see the
// old policy or the new one, never a mix.
class Firewall::RulesResource final : public bess::dataplane::Resource {
 public:
  explicit RulesResource(Firewall &owner)
      : Resource(owner.name() + "/rules"), owner_(owner) {}

  size_t LiveCount() const override { return owner_.rules_.size(); }

  bool Contains(const bess::dataplane::ResourceKey &key) const override {
    uint32_t id;
    return bess::dataplane::DecodeKey(key, &id) && owner_.rules_.contains(id);
  }

  std::expected<Reservation, std::string> Reserve(
      const bess::dataplane::Op &op) override {
    uint32_t id;
    if (!bess::dataplane::DecodeKey(op.key, &id)) {
      return std::unexpected("invalid rule id key");
    }
    // The engine refuses a second operation on one key, so the committed
    // rules answer whether this one existed.
    const bool existed = owner_.rules_.contains(id);
    Change change{id, {}, op.kind == bess::dataplane::OpKind::kErase};
    if (change.erase) {
      if (!existed) {
        return std::unexpected("not found");
      }
    } else {
      const KeyedRule *rule = std::any_cast<KeyedRule>(&op.value);
      if (rule == nullptr) {
        return std::unexpected("wrong value type (want a firewall rule)");
      }
      RuleMap source;
      source.emplace(id, *rule);
      change.node = source.extract(source.begin());
    }
    changes_.push_back(std::move(change));
    Reservation reservation{std::make_unique<SwapOp>(*this), {}, {}};
    // One swap per transaction retires one table: the first operation
    // declares it.
    reservation.footprint.retires = changes_.size() == 1 ? 1 : 0;
    reservation.existed = existed;
    return reservation;
  }

  std::expected<void, std::string> Prepare() override {
    // The committed rules and the changes, merged in id order: a change
    // replaces or removes the committed rule with its id.
    std::sort(changes_.begin(), changes_.end(),
              [](const Change &a, const Change &b) { return a.id < b.id; });
    std::vector<Ranked> ranked;
    ranked.reserve(owner_.rules_.size() + changes_.size());
    auto committed = owner_.rules_.cbegin();
    const auto end = owner_.rules_.cend();
    auto add = [&ranked](uint32_t id, const KeyedRule &rule) {
      ranked.push_back(Ranked{rule.priority, id, &rule.parsed});
    };
    for (const Change &c : changes_) {
      for (; committed != end && committed->first < c.id; ++committed) {
        add(committed->first, committed->second);
      }
      if (committed != end && committed->first == c.id) {
        ++committed;
      }
      if (!c.erase) {
        add(c.id, c.node.mapped());
      }
    }
    for (; committed != end; ++committed) {
      add(committed->first, committed->second);
    }
    auto published = BuildPublished(std::move(ranked));
    if (!published) {
      return std::unexpected(published.error());
    }
    prepared_ = std::move(*published);
    return {};
  }

  void EndTransaction() noexcept override {
    // Frees what the transaction replaced or erased, or what it never
    // published.
    changes_.clear();
    prepared_.reset();
  }

 private:
  // One operation. An upsert's node holds the new rule until the publish
  // moves it in (and then the rule it replaced); an erase's node receives the
  // erased rule.
  struct Change {
    uint32_t id;
    RuleMap::node_type node;
    bool erase;
  };

  // What every operation of a transaction stages: the first to publish swaps
  // the prepared policy and the rules in, the rest find nothing left to do.
  class SwapOp final : public bess::dataplane::StagedOp {
   public:
    explicit SwapOp(RulesResource &resource) : resource_(resource) {}
    void Publish(bess::dataplane::Retirer &retirer) noexcept override {
      if (resource_.prepared_ == nullptr) {
        return;
      }
      Firewall &fw = resource_.owner_;
      retirer.Retire(fw.published_.Exchange(std::move(resource_.prepared_)));
      for (Change &c : resource_.changes_) {
        if (c.erase) {
          c.node = fw.rules_.extract(c.id);
          continue;
        }
        auto it = fw.rules_.find(c.id);
        if (it == fw.rules_.end()) {
          fw.rules_.insert(std::move(c.node));
        } else {
          std::swap(it->second, c.node.mapped());
        }
      }
    }

   private:
    RulesResource &resource_;
  };

  Firewall &owner_;
  // During a transaction: its operations (in id order after Prepare()).
  std::vector<Change> changes_;
  // Built by Prepare(): the policy the publish installs.
  std::unique_ptr<const Published> prepared_;
};

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
  RuleMap rules;
  for (int i = 0; i < arg.rules_size(); i++) {
    auto parsed = ParseRule(arg.rules(i));
    if (!parsed) {
      return CommandFailure(EINVAL, "rule %d: %s", i, parsed.error().c_str());
    }
    rules.emplace(static_cast<uint32_t>(i + 1),
                  KeyedRule{0, arg.rules(i), *parsed});
  }
  std::vector<Ranked> ranked;
  ranked.reserve(rules.size());
  for (const auto &[id, rule] : rules) {
    ranked.push_back(Ranked{rule.priority, id, &rule.parsed});
  }
  auto published = BuildPublished(std::move(ranked));
  if (!published) {
    return CommandFailure(EINVAL, "%s", published.error().c_str());
  }
  published_.Initialize(std::move(*published));
  rules_ = std::move(rules);

  resource_ = std::make_unique<RulesResource>(*this);
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
            for (const auto &[id, rule] : rules_) {
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

bool Firewall::KeyOf(bess::PacketRef pkt, bess::classifier::FirewallKey *key) {
  using bess::classifier::FirewallFamily;
  using bess::packet::L3Kind;
  using bess::packet::L4Kind;
  using bess::packet::ParseStatus;

  // The checked parse ConnTrack, NAT and Router read (D-188): the headers in
  // the first segment, their lengths checked against the whole packet.
  bess::packet::ParsedFrame p;
  const ParseStatus status = bess::packet::ParseFrame(
      std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), p,
      pkt.total_len());
  // A fragment other than the first carries no transport header, so its
  // ports are unknown, but the IP header (IPv6: the fragment header) still
  // names the transport protocol: it is judged with ports 0, which only a
  // rule whose port range starts at 0 can match. Reading ports anyway would
  // judge it by whatever bytes follow.
  if (status != ParseStatus::kOk && status != ParseStatus::kFragment) {
    return false;  // not IP, or malformed: nothing to judge
  }
  key->family = p.l3 == L3Kind::kIpv6 ? FirewallFamily::kIpv6
                                      : FirewallFamily::kIpv4;
  key->proto = p.protocol;
  key->src = p.src;
  key->dst = p.dst;
  // Only TCP and UDP have ports to match: the parse's ICMP echo identifier
  // is not one.
  const bool ports = p.l4 == L4Kind::kTcp || p.l4 == L4Kind::kUdp;
  key->src_port = ports ? p.src_port : 0;
  key->dst_port = ports ? p.dst_port : 0;
  return true;
}

void Firewall::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::classifier::FirewallFamily;
  using bess::classifier::FirewallKey;
  using bess::classifier::FirewallTable;

  const Published *published = published_.Read();
  const FirewallTable *table =
      published == nullptr ? nullptr : published->table.get();
  // D-141: a packet this policy passes carries the action id its rule names,
  // so a module downstream resolves it; 0 when the rule names none, so a stale
  // id cannot reach a later lookup. The offset is valid only when something
  // reads the attribute; with no reader there is nothing to publish to.
  const bess::metadata::mt_offset_t action_offset = attr_offset(action_id_attr_);
  const bool publish_action = bess::metadata::IsValidOffset(action_offset);

  const size_t n = static_cast<size_t>(batch->cnt());
  std::array<FirewallKey, bess::PacketBatch::kMaxBurst> keys;
  std::array<FirewallTable::Match, bess::PacketBatch::kMaxBurst> matches;
  // A policy that names only one family can drop the other's packets without
  // a lookup (derived at build, so it cannot disagree with the rules).
  const bool ipv4_ok =
      table != nullptr && table->HasFamily(FirewallFamily::kIpv4);
  const bool ipv6_ok =
      table != nullptr && table->HasFamily(FirewallFamily::kIpv6);
  uint64_t parseable = 0;
  for (size_t i = 0; i < n; i++) {
    if (!KeyOf(batch->packet(i), &keys[i])) {
      keys[i] = FirewallKey{};
      continue;
    }
    const bool served = keys[i].family == FirewallFamily::kIpv6 ? ipv6_ok
                                                                : ipv4_ok;
    if (served) {
      parseable |= uint64_t{1} << i;
    }
  }

  // One table lookup for the whole batch; a packet this table cannot judge is
  // masked out of the result, so a catch-all rule cannot claim it.
  uint64_t matched = 0;
  if (table != nullptr && n > 0) {
    // Rule ids only when the action is published: the write needs the matched
    // rule's ActionId, and rule_id is what names the rule (0 otherwise).
    matched =
        table->LookupBatch(keys.data(), n, matches.data(), publish_action) &
        parseable;
  }

  for (size_t i = 0; i < n; i++) {
    bess::PacketRef pkt = batch->packet(i);
    const bool pass = ((matched >> i) & 1) != 0 && matches[i].action == kPass;
    if (pass) {
      if (publish_action) {
        // rule_id is 1-based; action_ids holds one entry per rule, in order.
        const uint32_t id = published->action_ids[matches[i].rule_id - 1];
        _set_attr_with_offset<bess::utils::be32_t>(action_offset, pkt,
                                                   bess::utils::be32_t(id));
      }
      EmitPacket(ctx, pkt, ctx->current_igate);
    } else {
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(Firewall, "firewall",
           "a priority-ordered 4/6 5-tuple policy with port ranges (first match wins)")
