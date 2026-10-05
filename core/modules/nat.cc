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
#include "worker.h"

namespace {

namespace nat = bess::nat;
using bess::utils::be32_t;

// Wheel work per batch.
constexpr size_t kExpireBudget = 256;
// Slots of the old table copied per batch while growing (TP5): 64K slots move
// in about 1K batches; the copy is a few tens of ns a binding.
constexpr size_t kMigrateSlots = 64;

}  // namespace

const Commands NAT::cmds = {
    {"get_initial_arg", "EmptyArg", MODULE_CMD_FUNC(&NAT::GetInitialArg),
     Command::THREAD_SAFE},
    // While workers run: the usage log's one consumer is the control side
    // (every module command holds the control-plane lock); a report request
    // is one atomic flag.
    {"request_usage_report", "EmptyArg", MODULE_CMD_FUNC(&NAT::CommandRequestUsageReport),
     Command::THREAD_SAFE},
    {"drain_usage", "EmptyArg", MODULE_CMD_FUNC(&NAT::CommandDrainUsage),
     Command::THREAD_SAFE},
    {"get_runtime_config", "EmptyArg", MODULE_CMD_FUNC(&NAT::GetRuntimeConfig),
     Command::THREAD_SAFE},
    {"set_runtime_config", "EmptyArg", MODULE_CMD_FUNC(&NAT::SetRuntimeConfig),
     Command::THREAD_SAFE}};

CommandResponse NAT::Init(const bess::pb::NATArg &arg) {
  table_full_ = bess::stats::EventThrottle(init_context().events(), "bess.table_full", name());
  ports_exhausted_ =
      bess::stats::EventThrottle(init_context().events(), "bess.nat_ports_exhausted", name());
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
  // 65,536 bindings (user decision 3-B, D-068) or what the addresses can
  // serve if less; with `max_capacity` larger, the table grows to it as it
  // fills (TP5, D-078). Owned is fixed by default (user decision 9.3).
  const size_t servable = nat::Nat::CapacityFor(config.addresses);
  config.capacity = std::min<size_t>(arg.capacity() != 0 ? arg.capacity() : 65536, servable);
  config.start = tsc_to_ns(rdtsc());
  config.seed = rdtsc();
  config.usage_log = arg.usage_log() != 0 ? arg.usage_log() : 4096;
  CommandResponse made;
  if (arg.shared()) {
    // Every worker translates through one NAT (TP6, D-079); it grows by
    // default, up to what the addresses serve (user decision 9.3).
    config.max_capacity =
        std::min<size_t>(arg.max_capacity() != 0 ? arg.max_capacity() : servable, servable);
    config.rcu = &init_context().rcu();
    made = arg.usage() ? Make<nat::CountedSharedNat>(config) : Make<nat::SharedNat>(config);
    max_allowed_workers_ = Worker::kMaxWorkers;
  } else {
    config.max_capacity = std::min<size_t>(arg.max_capacity(), servable);
    if (config.max_capacity > config.capacity) {
      made = arg.usage() ? Make<nat::CountedGrowableNat>(config) : Make<nat::GrowableNat>(config);
    } else {
      config.max_capacity = 0;
      made = arg.usage() ? Make<nat::CountedNat>(config) : Make<nat::Nat>(config);
    }
  }
  if (made.error().code() != 0) {
    return made;
  }
  grow_ = bess::framework::RequestEndpoint<GrowRequest>(
      init_context().requests(), [this](const GrowRequest &r) { OnGrowRequest(r); });
  free_ = bess::framework::RequestEndpoint<GrowRequest>(
      init_context().requests(), [this](const GrowRequest &r) { OnGrowRequest(r); });
  return CommandSuccess();
}

template <typename N>
CommandResponse NAT::Make(typename N::Config &config) {
  auto made = N::Create(config);
  if (!made) {
    return CommandFailure(ENOMEM, "cannot create the NAT binding table");
  }
  engine_ = std::move(*made);
  if constexpr (N::kGrowable && !N::kShared) {
    delete_table_ = [](void *t) { delete static_cast<typename N::Table *>(t); };
  }
  return CommandSuccess();
}

NAT::~NAT() {
  if (delete_table_ != nullptr) {
    delete_table_(handover_.exchange(nullptr));
    delete_table_(retired_.exchange(nullptr));
  }
}

// Control side, under the control-plane lock (the maintenance loop).
void NAT::OnGrowRequest(const GrowRequest &request) {
  if (request.capacity == 0) {
    delete_table_(retired_.exchange(nullptr, std::memory_order_acquire));
    free_.Done();
    return;
  }
  std::visit(
      [&](auto &engine) {
        using P = std::decay_t<decltype(engine)>;
        if constexpr (!std::is_same_v<P, std::monostate>) {
          using N = typename P::element_type;
          if constexpr (N::kShared) {
            // Shared: the whole growth runs here; workers keep translating (D-079).
            (void)engine->Grow(request.capacity);
            grow_.Done();
          } else if constexpr (N::kGrowable) {
            // A table retired by an earlier growth whose free request is still queued.
            delete_table_(retired_.exchange(nullptr, std::memory_order_acquire));
            auto bigger = N::NewTable(request.capacity);
            if (bigger == nullptr) {
              // No memory: the table stays as it is (creates refuse with kFull,
              // counted as drops); the next request asks again.
              grow_.Done();
              return;
            }
            delete_table_(handover_.exchange(bigger.release(), std::memory_order_release));
            // Re-armed once the worker has taken the table and finished
            // migrating (Run), so a full table asks once per growth.
          }
        }
      },
      engine_);
}

CommandResponse NAT::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::NATArg resp;
  const std::vector<nat::ExternalAddress> *addresses = nullptr;
  std::visit(
      [&](auto &engine) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(engine)>, std::monostate>) {
          addresses = &engine->addresses();
        }
      },
      engine_);
  for (const auto &a : *addresses) {
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

CommandResponse NAT::CommandRequestUsageReport(const bess::pb::EmptyArg &) {
  bool counted = false;
  std::visit(
      [&](auto &engine) {
        using P = std::decay_t<decltype(engine)>;
        if constexpr (!std::is_same_v<P, std::monostate>) {
          if constexpr (P::element_type::kUsage) {
            engine->RequestReport();
            counted = true;
          }
        }
      },
      engine_);
  return counted ? CommandSuccess() : CommandFailure(EINVAL, "this NAT has no usage counters (usage)");
}

CommandResponse NAT::CommandDrainUsage(const bess::pb::EmptyArg &) {
  bess::pb::NATUsageResponse resp;
  bool counted = false;
  std::visit(
      [&](auto &engine) {
        using P = std::decay_t<decltype(engine)>;
        if constexpr (!std::is_same_v<P, std::monostate>) {
          if constexpr (P::element_type::kUsage) {
            std::vector<nat::UsageRecord> records;
            engine->DrainUsage(&records, ~size_t{0});
            for (const nat::UsageRecord &r : records) {
              auto *out = resp.add_records();
              out->set_internal_addr(ToIpv4Address(r.internal.addr));
              out->set_internal_port(r.internal.port.value());
              out->set_external_addr(ToIpv4Address(r.external.addr));
              out->set_external_port(r.external.port.value());
              out->set_protocol(r.internal.protocol);
              out->set_packets(r.packets);
              out->set_bytes(r.bytes);
              out->set_final(r.final);
            }
            resp.set_reports_done(engine->reports_done());
            counted = true;
          }
        }
      },
      engine_);
  if (!counted) {
    return CommandFailure(EINVAL, "this NAT has no usage counters (usage)");
  }
  return CommandSuccess(resp);
}

void NAT::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  std::visit(
      [&](auto &engine) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(engine)>, std::monostate>) {
          Run(*engine, ctx, batch);
        }
      },
      engine_);
}

template <typename N>
void NAT::Run(N &nat, Context *ctx, bess::PacketBatch *batch) {
  if constexpr (N::kShared) {
    if (nat.NeedsGrowth()) [[unlikely]] {
      (void)grow_.Post({nat.GrowthTarget()});
    }
  } else if constexpr (N::kGrowable) {
    // Take a handed-over table, migrate a few bindings a batch, hand the old
    // table back to be freed, ask for a larger one when 3/4 full. A relaxed
    // load first: the exchange (a locked instruction) only when a table has
    // been handed over, once per growth.
    if (handover_.load(std::memory_order_relaxed) != nullptr) [[unlikely]] {
      nat.Adopt(std::unique_ptr<typename N::Table>(
          static_cast<typename N::Table *>(handover_.exchange(nullptr, std::memory_order_acquire))));
    }
    if (nat.migrating()) [[unlikely]] {
      if (auto old = nat.MigrateSome(kMigrateSlots)) {
        retired_.store(old.release(), std::memory_order_release);
        (void)free_.Post({0});
        grow_.Done();
      }
    } else if (nat.NeedsGrowth()) [[unlikely]] {
      (void)grow_.Post({nat.GrowthTarget()});
    }
  }
  nat.Expire(ctx->current_ns, kExpireBudget);  // shared: whichever worker gets the lock
  Translate(nat, ctx, batch);
}

template <typename N>
void NAT::Translate(N &nat, Context *ctx, bess::PacketBatch *batch) {
  const auto dir = ctx->current_igate == 0 ? nat::Direction::kForward
                                           : nat::Direction::kReverse;
  const gate_idx_t ogate = dir == nat::Direction::kForward ? 1 : 0;
  const uint64_t now = ctx->current_ns;
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
  nat.TranslateBatch(std::span(frames, cnt), std::span(parsed, cnt), std::span(ok, cnt), dir,
                     now, std::span(verdicts, cnt));
  // The common case, every packet translated: the batch leaves whole on one
  // gate (about 2.5 ns a packet less than emitting each; D-092).
  bool all = true;
  for (int i = 0; i < cnt; i++) {
    all &= verdicts[i] == nat::Verdict::kTranslated;
  }
  if (all) [[likely]] {
    RunChooseModule(ctx, ogate, batch);
    return;
  }
  uint64_t full = 0, exhausted = 0;
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    if (verdicts[i] == nat::Verdict::kTranslated) {
      EmitPacket(ctx, pkt, ogate);
    } else {
      full += verdicts[i] == nat::Verdict::kFull;
      exhausted += verdicts[i] == nat::Verdict::kExhausted;
      DropPacket(ctx, pkt);
    }
  }
  if ((full | exhausted) != 0) [[unlikely]] {
    if (full != 0) {
      table_full_.Note(ctx->wid, now, full);
    }
    if (exhausted != 0) {
      ports_exhausted_.Note(ctx->wid, now, exhausted);
    }
  }
}

std::string NAT::GetDesc() const {
  size_t n = 0;
  std::visit(
      [&](auto &engine) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(engine)>, std::monostate>) {
          n = engine->size();
        }
      },
      engine_);
  return bess::utils::Format("%zu entries", n);
}

ADD_MODULE(NAT, "nat", "Dynamic Network address/port translator")
