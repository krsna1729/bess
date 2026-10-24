// SPDX-License-Identifier: BSD-3-Clause

// Conformance plugin (D-191): both kinds of table resource, from the installed
// headers alone. Three tables each hold a rule for UDP destination port 1500,
// keyed by the port's two bytes:
//
//   - an in-place exact table: ConcurrentExactTable behind ExactRuleResource;
//   - an in-place masked table: ConcurrentMaskedTable behind
//     MaskedRuleResource;
//   - an immutable port-range table rebuilt once per transaction:
//     RebuildResource, published through an RcuPtr.
//
// Each is registered as "<name>/exact", "<name>/masked" and "<name>/ranges".
// A packet all three match leaves on gate 1; every other packet on gate 0.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "classifier/concurrent_exact.h"
#include "classifier/concurrent_masked.h"
#include "classifier/exact_rule_resource.h"
#include "classifier/masked_rule_resource.h"
#include "dataplane/rebuild_resource.h"
#include "dataplane/resource_registry.h"
#include "framework/plugin.h"
#include "module.h"
#include "rcu/rcu_ptr.h"

namespace {

constexpr size_t kUdpDstPortOffset = 14 + 20 + 2;  // Ethernet + IPv4 + UDP src
constexpr size_t kKeyLen = 2;
constexpr uint16_t kPort = 1500;
constexpr gate_idx_t kMatchGate = 1;

// One rule of the rebuilt table: a port range and the gate it selects.
struct PortRule {
  uint16_t low = 0;
  uint16_t high = 0;
  gate_idx_t gate = 0;
};

// The rules in id order; the first that holds the port wins.
struct PortTable {
  std::vector<PortRule> rules;

  gate_idx_t Lookup(uint16_t port) const noexcept {
    for (const PortRule &r : rules) {
      if (port >= r.low && port <= r.high) {
        return r.gate;
      }
    }
    return 0;
  }
};

using PortRules = bess::dataplane::RebuildRules<uint32_t, PortRule>;

std::expected<std::unique_ptr<const PortTable>, std::string> BuildPorts(
    const PortRules &rules) {
  auto table = std::make_unique<PortTable>();
  table->rules.reserve(rules.size());
  for (const auto &e : rules) {
    if (e.rule->low > e.rule->high) {
      return std::unexpected("rule " + std::to_string(e.id) + ": an empty range");
    }
    table->rules.push_back(*e.rule);
  }
  return table;
}

using PortsResource =
    bess::dataplane::RebuildResource<uint32_t, PortRule, PortTable, decltype(&BuildPorts)>;

}  // namespace

class StandaloneRuleTables final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;
  // The packet path only reads the tables.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  StandaloneRuleTables() : ports_(init_context().rcu()) {}

  CommandResponse Init(const bess::pb::EmptyArg &) {
    using bess::classifier::ConcurrentExactTable;
    using bess::classifier::ConcurrentMaskedTable;
    const std::array<std::byte, kKeyLen> key = {std::byte{kPort >> 8},
                                                std::byte{kPort & 0xff}};
    const std::array<std::byte, kKeyLen> all_ones = {std::byte{0xff}, std::byte{0xff}};

    auto exact = ConcurrentExactTable::Create(kKeyLen, ConcurrentExactTable::CapacityFor(16),
                                              init_context().rcu());
    if (!exact) {
      return CommandFailure(ENOMEM, "%s", exact.error().c_str());
    }
    exact_ = std::move(*exact);
    if (exact_->Upsert(key, bess::MakeGateResult(kMatchGate, 0)) ==
        ConcurrentExactTable::UpsertResult::kFull) {
      return CommandFailure(ENOMEM, "exact table full");
    }

    auto masked = ConcurrentMaskedTable::Create(kKeyLen, /*max_tuples=*/4, init_context().rcu());
    if (!masked) {
      return CommandFailure(ENOMEM, "%s", masked.error().c_str());
    }
    masked_ = std::move(*masked);
    if (masked_->Upsert(all_ones, key, /*priority=*/1, kMatchGate) !=
        ConcurrentMaskedTable::UpsertResult::kInserted) {
      return CommandFailure(EINVAL, "masked rule refused");
    }

    auto ports = std::make_unique<PortsResource>(name() + "/ranges", ports_, &BuildPorts,
                                                 "a port rule");
    if (auto initialized = ports->Initialize({{1, PortRule{1000, 2000, kMatchGate}}});
        !initialized) {
      return CommandFailure(EINVAL, "%s", initialized.error().c_str());
    }

    exact_resource_ =
        std::make_unique<bess::classifier::ExactRuleResource>(name() + "/exact", *exact_);
    masked_resource_ = std::make_unique<bess::classifier::MaskedRuleResource>(
        name() + "/masked", bess::classifier::MaskedRuleResource::Hooks{
                                .table = [this] { return masked_; },
                                .check_value = {},
                                .references = {},
                                .records = nullptr,
                                .may_reference = {},
                            });
    ports_resource_ = std::move(ports);
    for (bess::dataplane::Resource *r :
         {static_cast<bess::dataplane::Resource *>(exact_resource_.get()),
          static_cast<bess::dataplane::Resource *>(masked_resource_.get()),
          static_cast<bess::dataplane::Resource *>(ports_resource_.get())}) {
      if (auto registered = init_context().resources().Register(r); !registered) {
        Release();
        return CommandFailure(EEXIST, "%s", registered.error().c_str());
      }
      registered_.push_back(r->name());
    }
    return CommandSuccess();
  }

  void DeInit() override { Release(); }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const size_t n = static_cast<size_t>(batch->cnt());
    std::array<std::byte, bess::PacketBatch::kMaxBurst * kKeyLen> keys{};
    for (size_t i = 0; i < n; i++) {
      bess::PacketRef pkt = batch->packet(i);
      if (pkt.head_len() > kUdpDstPortOffset + 1) {
        const auto *bytes = pkt.head_data<const uint8_t *>();
        keys[i * kKeyLen] = std::byte{bytes[kUdpDstPortOffset]};
        keys[(i * kKeyLen) + 1] = std::byte{bytes[kUdpDstPortOffset + 1]};
      }
    }
    const bess::classifier::ConstBytes rows(keys.data(), n * kKeyLen);
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> exact_values;
    std::array<bess::classifier::ConcurrentMaskedTable::Match, bess::PacketBatch::kMaxBurst>
        masked_matches;
    const uint64_t exact_hits = bess::classifier::ExactRuleResource::VisibleHits(
        exact_->LookupBatch(rows, kKeyLen, exact_values.data(), n), exact_values.data());
    const uint64_t masked_hits =
        masked_->LookupBatch(rows, kKeyLen, masked_matches.data(), n, /*with_rule_id=*/false);
    const PortTable *ports = ports_.Read();
    for (size_t i = 0; i < n; i++) {
      const auto port = static_cast<uint16_t>(std::to_integer<unsigned>(keys[i * kKeyLen]) << 8U |
                                              std::to_integer<unsigned>(keys[(i * kKeyLen) + 1]));
      const bool all = ((exact_hits >> i) & 1) != 0 &&
                       bess::GateOfResult(exact_values[i]) == kMatchGate &&
                       ((masked_hits >> i) & 1) != 0 && masked_matches[i].gate == kMatchGate &&
                       ports->Lookup(port) == kMatchGate;
      EmitPacket(ctx, batch->packet(i), all ? kMatchGate : 0);
    }
    ProcessOGates(ctx);
  }

 private:
  void Release() {
    if (!registered_.empty()) {
      auto released = init_context().resources().ReleaseForTeardown(registered_);
      CHECK(released) << released.error();
      registered_.clear();
    }
    ports_resource_.reset();
    masked_resource_.reset();
    exact_resource_.reset();
  }

  // The tables before the resources over them, so the resources go first.
  std::unique_ptr<bess::classifier::ConcurrentExactTable> exact_;
  std::shared_ptr<bess::classifier::ConcurrentMaskedTable> masked_;
  bess::rcu::RcuPtr<PortTable> ports_;
  std::unique_ptr<bess::classifier::ExactRuleResource> exact_resource_;
  std::unique_ptr<bess::classifier::MaskedRuleResource> masked_resource_;
  std::unique_ptr<PortsResource> ports_resource_;
  std::vector<std::string> registered_;
};

const Commands StandaloneRuleTables::cmds = {};

BESS_PLUGIN("standalone_rule_tables", "1.0.0");

ADD_MODULE(StandaloneRuleTables, "standalone_rule_tables",
           "Conformance plugin: in-place and rebuilt table resources from installed headers")
