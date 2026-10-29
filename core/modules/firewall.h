// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_FIREWALL_H_
#define BESS_MODULES_FIREWALL_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "classifier/firewall_table.h"
#include "dataplane/rebuild_resource.h"
#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

// A firewall over the 5-tuple with port ranges: the policy of
// bess::classifier::FirewallTable, exposed as a module. The table is the
// library - any module can hold one and look up keys itself - and this module
// owns only the packet path, the "<name>/rules" resource and the
// drop-or-forward decision.
class Firewall final : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;
  // The packet path only reads the published policy.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  // What a rule carries in the table. The library returns the action
  // untouched, so the module names its own: kDrop drops the packet, kPass
  // forwards it on the incoming gate.
  static constexpr uint16_t kDrop = 0xFFFF;
  static constexpr uint16_t kPass = 0;

  Firewall() : Module(), published_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::FirewallArg &arg);
  void DeInit() override;

  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // The packet's 5-tuple, from the checked parse (packet_parse.h, D-188): up
  // to two VLAN tags, IPv4 options, up to eight IPv6 extension headers (AH
  // included). False for a packet this table cannot judge: not IPv4 or IPv6,
  // or malformed (a header cut short, lengths that lie, an EtherType its IP
  // version contradicts, more extension headers than the walk follows). A
  // packet without ports - a non-first fragment, ICMP, a protocol other than
  // TCP and UDP - is judged with both ports 0, which only a rule whose port
  // range starts at 0 can match. The common frame is read at its fixed
  // offsets after the parse's own checks (CheckFastIpv4), so it decides as
  // ParseFrame would. `frame` and `total_len` as ParseFrame's.
  static bool KeyOf(std::span<const uint8_t> frame, size_t total_len,
                    bess::classifier::FirewallKey *key);

 private:
  // One immutable rule set, published whole: changing the policy publishes a
  // new table and the old one is retired a grace period later, so workers
  // keep running (no paused workers) - the shape ACL's rule list uses
  // (D-017).
  struct Published {
    // The table's and action_ids' admission (D-198), "rules": taken before
    // they are built, released with this object -- after the grace period
    // when it is retired, at once when a transaction never publishes it.
    // Before `table`, so it is released after the table is freed.
    bess::framework::MemoryLease lease;
    std::unique_ptr<const bess::classifier::FirewallTable> table;
    // The ActionId of the table's rule k at index k - 1, so the packet path
    // names the action a matching rule carries (D-141) without holding the
    // rule list itself.
    std::vector<uint32_t> action_ids;
  };
  // A "<name>/rules" entry: its value as given (read back unchanged) and the
  // rule it converts to.
  struct KeyedRule {
    int64_t priority = 0;
    bess::pb::FirewallArg::Rule given;
    bess::classifier::FirewallRule parsed;
  };
  using RuleView = bess::dataplane::RebuildRules<uint32_t, KeyedRule>;

  // A rule as the policy orders it.
  struct Ranked {
    int64_t priority;
    uint32_t id;
    const bess::classifier::FirewallRule *rule;
  };

  // The table `rules` (in id order) make, in policy order: priority, highest
  // first, equal priorities by id, lowest first. Admitted as "rules" before
  // it is built (D-198: refusal throws utils::MemoryRefused, builds nothing).
  std::expected<std::unique_ptr<const Published>, std::string> BuildPublished(
      const RuleView &rules) const;
  // The rebuild callable: BuildPublished on this module's memory scope.
  struct Builder {
    const Firewall *owner;
    std::expected<std::unique_ptr<const Published>, std::string> operator()(
        const RuleView &rules) const {
      return owner->BuildPublished(rules);
    }
  };

  // The module's "<name>/rules" resource (D-179): keyed by rule id, the
  // policy rebuilt once per transaction (D-191). It holds the committed rules
  // by id: what the published table was built from, and what it reads back.
  using RulesResource =
      bess::dataplane::RebuildResource<uint32_t, KeyedRule, Published, Builder>;

  bess::rcu::RcuPtr<Published> published_;
  std::unique_ptr<RulesResource> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)
  // The packet's "action_id" (D-141): written for every packet the policy
  // passes, 0 when its rule names no action, so a module downstream selects
  // the action and a stale id cannot leak into a later lookup.
  int action_id_attr_ = -1;
  // The registered output name, for the log line that names it.
  std::string action_attr_name_;
  // Whether action_attr was configured, so the default name (a firewall with
  // nothing downstream) is quiet and a configured name with no reader is
  // reported.
  bool action_attr_configured_ = false;
};

#endif  // BESS_MODULES_FIREWALL_H_
