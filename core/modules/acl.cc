// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "acl.h"

#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "packet_parse.h"

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
    const be16_t src_port(static_cast<uint16_t>(rule.src_port()));
    const be16_t dst_port(static_cast<uint16_t>(rule.dst_port()));
    out->push_back({.src_ip = *src,
                    .dst_ip = *dst,
                    .src_port = src_port,
                    .dst_port = dst_port,
                    .drop = rule.drop()});
    i++;
  }
  return CommandSuccess();
}

ACL::Rules ACL::Rules::Compile(const std::vector<ACLRule> &rules) {
  // The masks and values as the key's bytes: each field where the frame
  // has it, so the key is two loads from the frame on every target.
  const auto pack = [](be32_t src, be32_t dst) {
    uint32_t half[2];
    std::memcpy(&half[0], &src, sizeof(src));
    std::memcpy(&half[1], &dst, sizeof(dst));
    uint64_t v;
    std::memcpy(&v, half, sizeof(v));
    return v;
  };
  const auto pack_ports = [](be16_t src, be16_t dst) {
    uint16_t half[2];
    std::memcpy(&half[0], &src, sizeof(src));
    std::memcpy(&half[1], &dst, sizeof(dst));
    uint32_t v;
    std::memcpy(&v, half, sizeof(v));
    return v;
  };
  const auto port_mask = [](be16_t port) { return be16_t(port == be16_t(0) ? 0 : 0xffff); };

  Rules out;
  out.named.reserve(rules.size());
  for (const ACLRule &r : rules) {
    const MatchRule m{
        .addr_mask = pack(r.src_ip.mask, r.dst_ip.mask),
        .addr_want = pack(r.src_ip.addr & r.src_ip.mask, r.dst_ip.addr & r.dst_ip.mask),
        .port_mask = pack_ports(port_mask(r.src_port), port_mask(r.dst_port)),
        .port_want = pack_ports(r.src_port, r.dst_port),
        .pass = !r.drop,
    };
    if (m.addr_mask == 0 && m.port_mask == 0) {
      out.otherwise_pass = m.pass;
      break;
    }
    out.named.push_back(m);
  }
  return out;
}

void ACL::Install(const std::vector<ACLRule> &rules) {
  auto next = std::make_unique<const Rules>(Rules::Compile(rules));
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
  Install(rules);
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
        return Staged{std::make_unique<const Rules>(Rules::Compile(parsed)), v};
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

namespace {

// KeyOf's verdict on a frame.
enum class Frame : uint8_t {
  kIpv4,       // the key is filled: every rule can match
  kNotIpv4,    // a valid frame that is not IPv4 (ARP, IPv6, ...): only a
               // rule with every field a wildcard matches
  kMalformed,  // headers that do not fit the frame or lie about their
               // lengths: dropped before any rule
};

// The frame's kind and, for IPv4, its key (ACL::Key's fields). 16 bytes, so
// it is returned in two registers.
struct Keyed {
  uint64_t addrs;
  uint32_t ports;
  Frame frame;
};
static_assert(sizeof(Keyed) == 16);

// KeyOf's path for every frame but the common one: the checked parse
// (D-188), out of line so the common frame's path stays small enough to
// inline into the batch loop.
[[gnu::noinline]] Keyed KeyOfParsed(std::span<const uint8_t> frame, size_t total_len) {
  using bess::packet::ParseStatus;
  bess::packet::ParsedFrame p;
  const ParseStatus status = bess::packet::ParseFrame(frame, p, total_len);
  if (status == ParseStatus::kMalformed) {
    return {.addrs = 0, .ports = 0, .frame = Frame::kMalformed};
  }
  if (status == ParseStatus::kNotIp || p.l3 != bess::packet::L3Kind::kIpv4) {
    return {.addrs = 0, .ports = 0, .frame = Frame::kNotIpv4};
  }
  Keyed k{.addrs = 0, .ports = 0, .frame = Frame::kIpv4};
  uint8_t addrs[8];
  std::memcpy(&addrs[0], p.src.data(), 4);
  std::memcpy(&addrs[4], p.dst.data(), 4);
  std::memcpy(&k.addrs, addrs, sizeof(k.addrs));
  if (p.l4 == bess::packet::L4Kind::kTcp || p.l4 == bess::packet::L4Kind::kUdp) {
    const be16_t ports[2] = {be16_t(p.src_port), be16_t(p.dst_port)};
    std::memcpy(&k.ports, ports, sizeof(k.ports));
  }
  return k;
}

// The common frame is read at its fixed offsets after CheckFastIpv4, so it
// decides as ParseFrame would. `frame` and `total_len` as ParseFrame's.
// Inline: the key stays in registers.
inline Keyed KeyOf(std::span<const uint8_t> frame, size_t total_len) {
  using bess::packet::ParseStatus;
  const bess::packet::FastIpv4 f = bess::packet::CheckFastIpv4(frame, total_len);
  if (!f.taken) [[unlikely]] {
    return KeyOfParsed(frame, total_len);
  }
  if (f.status == ParseStatus::kMalformed) [[unlikely]] {
    return {.addrs = 0, .ports = 0, .frame = Frame::kMalformed};
  }
  Keyed k{.addrs = 0, .ports = 0, .frame = Frame::kIpv4};
  // The source and the destination are adjacent: one load.
  std::memcpy(&k.addrs, frame.data() + bess::packet::kFastIpv4Src, sizeof(k.addrs));
  // A non-first fragment has no transport header (kFragment): ports 0. The
  // parse checked the 8 (UDP) or 20 (TCP) header bytes it would read.
  if (f.status == ParseStatus::kOk && (f.protocol == 6 || f.protocol == 17)) [[likely]] {
    std::memcpy(&k.ports, frame.data() + bess::packet::kFastIpv4L4, sizeof(k.ports));
  }
  return k;
}

}  // namespace

// A rule constrains only the fields it names: a rule with every field a
// wildcard matches any frame, IPv4 or not; any other rule matches only an
// IPv4 frame. A malformed frame is dropped before any rule.
void ACL::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  gate_idx_t incoming_gate = ctx->current_igate;
  // One rule list for the whole batch.
  const Rules &rules = *rules_.Read();
  const std::span<const MatchRule> named(rules.named);

  for (bess::PacketRef pkt : batch->packets()) {
    const Keyed k =
        KeyOf(std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), pkt.total_len());
    bool pass = rules.otherwise_pass;
    if (k.frame == Frame::kIpv4) [[likely]] {
      const Key key{.addrs = k.addrs, .ports = k.ports};
      for (const MatchRule &rule : named) {
        if (rule.Match(key)) {
          pass = rule.pass;
          break;  // Stop matching other rules
        }
      }
    } else if (k.frame == Frame::kMalformed) {
      pass = false;
    }

    if (pass) {
      EmitPacket(ctx, pkt, incoming_gate);
    } else {
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(ACL, "acl", "ACL module from NetBricks")
