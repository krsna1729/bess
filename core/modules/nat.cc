// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "nat.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "conntrack/packet_parse.h"
#include "utils/format.h"
#include "utils/ip.h"
#include "utils/time.h"

namespace {

namespace nat = bess::nat;
using bess::utils::be32_t;

// Wheel work per batch.
constexpr size_t kExpireBudget = 256;

}  // namespace

const Commands NAT::cmds = {
    {"get_initial_arg", "EmptyArg", MODULE_CMD_FUNC(&NAT::GetInitialArg),
     Command::THREAD_SAFE},
    {"get_runtime_config", "EmptyArg", MODULE_CMD_FUNC(&NAT::GetRuntimeConfig),
     Command::THREAD_SAFE},
    {"set_runtime_config", "EmptyArg", MODULE_CMD_FUNC(&NAT::SetRuntimeConfig),
     Command::THREAD_SAFE}};

CommandResponse NAT::Init(const bess::pb::NATArg &arg) {
  // Check before committing any changes.
  for (const auto &address_range : arg.ext_addrs()) {
    for (const auto &range : address_range.port_ranges()) {
      if (range.begin() >= range.end() || range.begin() > UINT16_MAX ||
          range.end() > UINT16_MAX) {
        return CommandFailure(EINVAL, "Port range for address %s is malformed",
                              address_range.ext_addr().c_str());
      }
    }
  }

  nat::Nat::Config config;
  for (const auto &address_range : arg.ext_addrs()) {
    nat::ExternalAddress ext;
    if (!bess::utils::ParseIpv4Address(address_range.ext_addr(), &ext.addr)) {
      return CommandFailure(EINVAL, "invalid IP address %s",
                            address_range.ext_addr().c_str());
    }
    if (address_range.port_ranges().size() == 0) {
      ext.ranges.push_back({0, 65535, false});
    }
    for (const auto &range : address_range.port_ranges()) {
      // Control plane gets to decide if the port range can be used.
      ext.ranges.push_back({static_cast<uint16_t>(range.begin()), range.end(),
                            range.suspended()});
    }
    config.addresses.push_back(std::move(ext));
  }
  if (config.addresses.empty()) {
    return CommandFailure(EINVAL,
                          "at least one external IP address must be specified");
  }
  // Sorted (with their ranges) so GetInitialArg is predictable and an internal
  // address maps to the same external one as before (the hash indexes this
  // order).
  std::stable_sort(config.addresses.begin(), config.addresses.end(),
                   [](const auto &a, const auto &b) { return a.addr < b.addr; });
  // As many bindings as the addresses' ports can serve, at most 1M: the
  // table is allocated here (about 130 bytes a binding, D-068).
  config.capacity = nat::Nat::CapacityFor(config.addresses);
  config.start = tsc_to_ns(rdtsc());
  config.seed = rdtsc();
  auto made = nat::Nat::Create(config);
  if (!made) {
    return CommandFailure(ENOMEM, "cannot create the NAT binding table");
  }
  nat_ = std::move(*made);
  return CommandSuccess();
}

CommandResponse NAT::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::NATArg resp;
  for (const auto &a : nat_->addresses()) {
    auto ext = resp.add_ext_addrs();
    ext->set_ext_addr(ToIpv4Address(a.addr));
    for (const auto &r : a.ranges) {
      auto erange = ext->add_port_ranges();
      erange->set_begin(r.begin);
      erange->set_end(r.end);
      erange->set_suspended(r.suspended);
    }
  }
  return CommandSuccess(resp);
}

CommandResponse NAT::GetRuntimeConfig(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

CommandResponse NAT::SetRuntimeConfig(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

void NAT::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const auto dir = ctx->current_igate == 0 ? nat::Direction::kForward
                                           : nat::Direction::kReverse;
  const gate_idx_t ogate = dir == nat::Direction::kForward ? 1 : 0;
  const uint64_t now = ctx->current_ns;
  nat_->Expire(now, kExpireBudget);
  const int cnt = batch->cnt();
  std::span<uint8_t> frames[bess::PacketBatch::kMaxBurst];
  bess::conntrack::ParsedFlowPacket parsed[bess::PacketBatch::kMaxBurst];
  bool ok[bess::PacketBatch::kMaxBurst];
  nat::Verdict verdicts[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    frames[i] = std::span<uint8_t>(pkt.head_data<uint8_t *>(), pkt.head_len());
    // The headers must be in the first segment; the lengths are checked
    // against the whole packet (a chained packet is translated in place).
    ok[i] = bess::conntrack::ParseFrame(frames[i], parsed[i], pkt.total_len()) ==
            bess::conntrack::ParseStatus::kOk;
  }
  nat_->TranslateBatch(std::span(frames, cnt), std::span(parsed, cnt),
                       std::span(ok, cnt), dir, now, std::span(verdicts, cnt));
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    if (verdicts[i] == nat::Verdict::kTranslated) {
      EmitPacket(ctx, pkt, ogate);
    } else {
      DropPacket(ctx, pkt);
    }
  }
}

std::string NAT::GetDesc() const {
  return bess::utils::Format("%zu entries", nat_ ? nat_->size() : 0);
}

ADD_MODULE(NAT, "nat", "Dynamic Network address/port translator")
