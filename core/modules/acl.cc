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
                    .drop = rule.drop(),
                    .any = src->mask == be32_t(0) && dst->mask == be32_t(0) &&
                           src_port == be16_t(0) && dst_port == be16_t(0)});
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

namespace {

// KeyOf's path for every frame but the common one: the checked parse
// (D-188), out of line so the common frame's path stays small enough to
// inline into the batch loop.
[[gnu::noinline]] ACL::Frame KeyOfParsed(std::span<const uint8_t> frame, size_t total_len,
                                         ACL::Key *key) {
  using bess::packet::ParseStatus;
  bess::packet::ParsedFrame p;
  const ParseStatus status = bess::packet::ParseFrame(frame, p, total_len);
  if (status == ParseStatus::kMalformed) {
    return ACL::Frame::kMalformed;
  }
  if (status == ParseStatus::kNotIp || p.l3 != bess::packet::L3Kind::kIpv4) {
    return ACL::Frame::kNotIpv4;
  }
  std::memcpy(&key->src, p.src.data(), sizeof(key->src));
  std::memcpy(&key->dst, p.dst.data(), sizeof(key->dst));
  const bool ports = p.l4 == bess::packet::L4Kind::kTcp || p.l4 == bess::packet::L4Kind::kUdp;
  key->src_port = be16_t(ports ? p.src_port : 0);
  key->dst_port = be16_t(ports ? p.dst_port : 0);
  return ACL::Frame::kIpv4;
}

}  // namespace

ACL::Frame ACL::KeyOf(std::span<const uint8_t> frame, size_t total_len, Key *key) {
  using bess::packet::ParseStatus;
  // The common frame: the parse's checks, then the fields at their offsets.
  const bess::packet::FastIpv4 f = bess::packet::CheckFastIpv4(frame, total_len);
  if (!f.taken) [[unlikely]] {
    return KeyOfParsed(frame, total_len, key);
  }
  if (f.status == ParseStatus::kMalformed) {
    return Frame::kMalformed;
  }
  std::memcpy(&key->src, frame.data() + bess::packet::kFastIpv4Src, sizeof(key->src));
  std::memcpy(&key->dst, frame.data() + bess::packet::kFastIpv4Dst, sizeof(key->dst));
  // A non-first fragment has no transport header (kFragment): ports 0. The
  // parse checked the 8 (UDP) or 20 (TCP) header bytes it would read.
  key->src_port = be16_t(0);
  key->dst_port = be16_t(0);
  if (f.status == ParseStatus::kOk && (f.protocol == 6 || f.protocol == 17)) {
    const uint8_t *l4 = frame.data() + bess::packet::kFastIpv4L4;
    std::memcpy(&key->src_port, l4, sizeof(key->src_port));
    std::memcpy(&key->dst_port, l4 + 2, sizeof(key->dst_port));
  }
  return Frame::kIpv4;
}

// A rule constrains only the fields it names: a rule with every field a
// wildcard matches any frame, IPv4 or not; any other rule matches only an
// IPv4 frame. A malformed frame is dropped before any rule.
void ACL::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  gate_idx_t incoming_gate = ctx->current_igate;
  // One rule list for the whole batch.
  const std::vector<ACLRule> &rules = rules_.Read()->rules;

  for (bess::PacketRef pkt : batch->packets()) {
    Key key;
    const Frame frame = KeyOf(std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()),
                              pkt.total_len(), &key);
    if (frame == Frame::kMalformed) [[unlikely]] {
      DropPacket(ctx, pkt);
      continue;
    }
    const bool ipv4 = frame == Frame::kIpv4;

    bool emitted = false;
    for (const auto &rule : rules) {
      if (ipv4 ? rule.Match(key.src, key.dst, key.src_port, key.dst_port) : rule.any) {
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
