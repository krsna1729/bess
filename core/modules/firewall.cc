// SPDX-License-Identifier: BSD-3-Clause

#include "firewall.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/logging.h"
#include "utils/tcp.h"
#include "utils/udp.h"

// D-129: "<name>/rules" (a singleton) holds the whole ordered policy; the
// class has no actions.
const Commands Firewall::cmds = {};

const ResourceTables Firewall::resource_tables = {
    ResourceTable::Singleton<bess::pb::FirewallArg>("rules", false)};

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

// IPv6's fixed header, by offset (40 bytes; the addresses follow it).
constexpr size_t kIpv6HeaderBytes = 40;
constexpr size_t kIpv6NextHeader = 6;
constexpr size_t kIpv6Src = 8;
constexpr size_t kIpv6Dst = 24;
// How many extension headers a packet may carry before this table stops
// looking for the transport header.
constexpr int kMaxIpv6ExtHeaders = 4;

// The ActionId of each rule, in order: what the packet path publishes for a
// matching packet (D-141), kept beside the table so a worker never walks the
// rule list.
std::vector<uint32_t> ActionIdsOf(
    const std::vector<bess::classifier::FirewallRule> &rules) {
  std::vector<uint32_t> ids;
  ids.reserve(rules.size());
  for (const auto &rule : rules) {
    ids.push_back(rule.action_id);
  }
  return ids;
}

}  // namespace

CommandResponse Firewall::ParseRules(
    const bess::pb::FirewallArg &arg,
    std::vector<bess::classifier::FirewallRule> *out) const {
  using bess::classifier::FirewallRule;
  out->clear();
  int i = 0;
  for (const auto &rule : arg.rules()) {
    // "" is a wildcard (documented): the field matches any address of the
    // rule's family.
    std::optional<Cidr> src;
    std::optional<Cidr> dst;
    if (!rule.src_ip().empty()) {
      src = ParseCidr(rule.src_ip());
      if (!src) {
        return CommandFailure(EINVAL, "rule %d: invalid IP prefix '%s'", i,
                              rule.src_ip().c_str());
      }
    }
    if (!rule.dst_ip().empty()) {
      dst = ParseCidr(rule.dst_ip());
      if (!dst) {
        return CommandFailure(EINVAL, "rule %d: invalid IP prefix '%s'", i,
                              rule.dst_ip().c_str());
      }
    }
    // The prefixes decide the rule's family. A rule with neither matches both
    // families; "any IPv4" is "0.0.0.0/0" and "any IPv6" is "::/0".
    FirewallFamily family = FirewallFamily::kAny;
    if (src && dst) {
      if (src->family != dst->family) {
        return CommandFailure(
            EINVAL, "rule %d: '%s' and '%s' are different families", i,
            rule.src_ip().c_str(), rule.dst_ip().c_str());
      }
      family = src->family;
    } else if (src) {
      family = src->family;
    } else if (dst) {
      family = dst->family;
    }

    if (rule.proto() > 0xff) {
      return CommandFailure(EINVAL, "rule %d: protocol %u is out of range", i,
                            rule.proto());
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
      return CommandFailure(EINVAL, "rule %d: a port is out of range", i);
    }

    FirewallRule parsed;
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
    if (rule.drop() && rule.action_id() != 0) {
      return CommandFailure(
          EINVAL,
          "rule %d: a rule that drops cannot name an action - the packet "
          "never reaches a module that would resolve it",
          i);
    }
    parsed.action = rule.drop() ? kDrop : kPass;
    parsed.action_id = rule.action_id();
    out->push_back(parsed);
    i++;
  }
  return CommandSuccess();
}

void Firewall::Install(std::unique_ptr<const Published> published) {
  if (rules_.Read() == nullptr) {
    rules_.Initialize(std::move(published));
    return;
  }
  rules_.Publish(std::move(published));
  init_context().rcu().ReclaimReady();
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
  std::vector<bess::classifier::FirewallRule> rules;
  CommandResponse ret = ParseRules(arg, &rules);
  if (ret.has_error()) {
    return ret;
  }
  std::vector<uint32_t> action_ids = ActionIdsOf(rules);
  auto table = bess::classifier::FirewallTable::Build(std::move(rules));
  if (!table) {
    return CommandFailure(EINVAL, "%s", table.error().c_str());
  }
  Install(std::make_unique<const Published>(
      Published{std::move(*table), std::move(action_ids)}));
  arg_ = arg;

  // Stage builds the whole table (a refused rule refuses the transaction);
  // apply swaps the published table and retires the old one a grace period
  // later, so workers keep running.
  auto registered = rules_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/rules",
      // clang-tidy 19's analyzer loses the unique_ptr moved into the returned
      // std::expected and reports a leak at the closing brace.
      // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
      [this](const bess::pb::FirewallArg &v) -> std::expected<Staged, std::string> {
        std::vector<bess::classifier::FirewallRule> parsed;
        CommandResponse r = ParseRules(v, &parsed);
        if (r.has_error()) {
          return std::unexpected(r.error().errmsg());
        }
        std::vector<uint32_t> action_ids = ActionIdsOf(parsed);
        auto next = bess::classifier::FirewallTable::Build(std::move(parsed));
        if (!next) {
          return std::unexpected(next.error());
        }
        return Staged{
            std::make_unique<const Published>(
                Published{std::move(*next), std::move(action_ids)}),
            v};
      },
      // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
      [this](Staged &s, bess::dataplane::Retirer &retirer) {
        retirer.Retire(rules_.Exchange(std::move(s.published)));
        // The resource carries the policy; action_attr registers the read at
        // Init and cannot change at runtime. Keeping the configured name in
        // the read-back stops a rules-only set from erasing it from the
        // desired state.
        s.arg.set_action_attr(arg_.action_attr());
        arg_.Swap(&s.arg);
      },
      bess::dataplane::Resource::Footprint{.retires = 1},
      [this] { return arg_; }, nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Firewall::DeInit() {
  rules_resource_.Release();
}

bool Firewall::KeyOf(bess::PacketRef pkt, bess::classifier::FirewallKey *key) {
  using bess::classifier::FirewallFamily;
  using bess::utils::be16_t;
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Tcp;
  using bess::utils::Udp;

  const size_t len = pkt.data_len();
  if (len < sizeof(Ethernet)) {
    return false;
  }
  const uint8_t *data = pkt.head_data<const uint8_t *>();
  const Ethernet *eth = reinterpret_cast<const Ethernet *>(data);
  const uint8_t *payload = data + sizeof(Ethernet);
  const size_t payload_bytes = len - sizeof(Ethernet);

  key->src_port = 0;
  key->dst_port = 0;

  if (eth->ether_type == be16_t(Ethernet::Type::kIpv4)) {
    if (payload_bytes < sizeof(Ipv4)) {
      return false;
    }
    const Ipv4 *ip = reinterpret_cast<const Ipv4 *>(payload);
    const size_t ip_bytes = static_cast<size_t>(ip->header_length) << 2;
    if (ip_bytes < sizeof(Ipv4) || ip_bytes > payload_bytes) {
      return false;
    }
    key->family = FirewallFamily::kIpv4;
    key->proto = ip->protocol;
    std::memcpy(key->src.data(), &ip->src, 4);
    std::memcpy(key->dst.data(), &ip->dst, 4);

    // A fragment other than the first carries no transport header, so its
    // ports are unknown: the ports stay 0, which only a rule whose port range
    // starts at 0 can match. Reading them anyway would judge the fragment by
    // whatever bytes follow.
    if ((ip->fragment_offset.value() & 0x1fff) != 0) {
      return true;
    }
    if (ip->protocol != Ipv4::Proto::kTcp && ip->protocol != Ipv4::Proto::kUdp) {
      return true;
    }
    const size_t l4_bytes =
        ip->protocol == Ipv4::Proto::kTcp ? sizeof(Tcp) : sizeof(Udp);
    if (ip_bytes + l4_bytes > payload_bytes) {
      return false;
    }
    // Both Tcp and Udp begin with the two ports.
    const uint8_t *l4 = payload + ip_bytes;
    be16_t src_port;
    be16_t dst_port;
    std::memcpy(&src_port, l4, sizeof(be16_t));
    std::memcpy(&dst_port, l4 + sizeof(be16_t), sizeof(be16_t));
    key->src_port = src_port.value();
    key->dst_port = dst_port.value();
    return true;
  }

  if (eth->ether_type != be16_t(Ethernet::Type::kIpv6)) {
    return false;
  }
  if (payload_bytes < kIpv6HeaderBytes) {
    return false;
  }
  key->family = FirewallFamily::kIpv6;
  std::memcpy(key->src.data(), payload + kIpv6Src, 16);
  std::memcpy(key->dst.data(), payload + kIpv6Dst, 16);

  // Walk the extension chain (bounded) to the transport header: a rule that
  // names TCP must see the packet's transport protocol, not its first next
  // header.
  uint8_t next = payload[kIpv6NextHeader];
  size_t offset = kIpv6HeaderBytes;
  for (int hops = 0; hops < kMaxIpv6ExtHeaders; hops++) {
    if (next == Ipv4::Proto::kTcp || next == Ipv4::Proto::kUdp) {
      key->proto = next;
      if (offset + 4 > payload_bytes) {
        return false;
      }
      be16_t src_port;
      be16_t dst_port;
      std::memcpy(&src_port, payload + offset, sizeof(be16_t));
      std::memcpy(&dst_port, payload + offset + sizeof(be16_t), sizeof(be16_t));
      key->src_port = src_port.value();
      key->dst_port = dst_port.value();
      return true;
    }

    size_t header_bytes = 0;
    switch (next) {
      case 0:    // hop-by-hop options
      case 43:   // routing
      case 60:   // destination options
        if (offset + 8 > payload_bytes) {
          return false;
        }
        header_bytes = (static_cast<size_t>(payload[offset + 1]) + 1) * 8;
        break;
      case 44: {  // fragment
        if (offset + 8 > payload_bytes) {
          return false;
        }
        const uint16_t fragment_offset =
            static_cast<uint16_t>((payload[offset + 2] << 8) | payload[offset + 3]);
        if ((fragment_offset & 0xfff8) != 0) {
          // Not the first fragment: no transport header to read, but the
          // fragment header still names the transport protocol, so a rule
          // that asks for TCP sees the fragment and only its ports are
          // unknown.
          key->proto = payload[offset];
          return true;
        }
        header_bytes = 8;
        break;
      }
      case 51:  // authentication header: its length is in 4-byte units
        if (offset + 8 > payload_bytes) {
          return false;
        }
        header_bytes = (static_cast<size_t>(payload[offset + 1]) + 2) * 4;
        break;
      case 50:  // ESP: everything after it is encrypted
        key->proto = next;
        return true;
      default:
        // Anything else is either the transport itself or something this
        // table does not look inside: the protocol is that value and the
        // ports stay unknown.
        key->proto = next;
        return true;
    }
    if (offset + header_bytes > payload_bytes) {
      return false;
    }
    next = payload[offset];  // an extension header starts with its successor
    offset += header_bytes;
  }
  key->proto = next;
  // The transport may sit exactly at the bound: four extension headers and
  // then the transport header is still the transport header, and the loop
  // above has already spent its four looks at extension headers.
  if (next == Ipv4::Proto::kTcp || next == Ipv4::Proto::kUdp) {
    if (offset + 4 > payload_bytes) {
      return false;
    }
    be16_t src_port;
    be16_t dst_port;
    std::memcpy(&src_port, payload + offset, sizeof(be16_t));
    std::memcpy(&dst_port, payload + offset + sizeof(be16_t), sizeof(be16_t));
    key->src_port = src_port.value();
    key->dst_port = dst_port.value();
  }
  return true;
}

void Firewall::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::classifier::FirewallFamily;
  using bess::classifier::FirewallKey;
  using bess::classifier::FirewallTable;

  const Published *published = rules_.Read();
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
           "an ordered 4/6 5-tuple policy with port ranges (first match wins)")
