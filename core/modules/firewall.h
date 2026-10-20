// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_FIREWALL_H_
#define BESS_MODULES_FIREWALL_H_

#include <memory>
#include <vector>

#include "classifier/firewall_table.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

// A firewall over the 5-tuple with port ranges: the ordered policy of
// bess::classifier::FirewallTable, exposed as a module. The table is the
// library - any module can hold one and look up keys itself - and this module
// owns only the packet path, the "<name>/rules" resource and the
// drop-or-forward decision.
class Firewall final : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;

  // What a rule carries in the table. The library returns the action
  // untouched, so the module names its own: kDrop drops the packet, kPass
  // forwards it on the incoming gate.
  static constexpr uint16_t kDrop = 0xFFFF;
  static constexpr uint16_t kPass = 0;

  Firewall() : Module(), rules_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::FirewallArg &arg);
  void DeInit() override;

  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  // One immutable rule set, published whole: replacing the policy publishes a
  // new table and the old one is retired a grace period later, so workers
  // keep running (no paused workers) - the shape ACL's rule list uses
  // (D-017).
  struct Published {
    std::unique_ptr<const bess::classifier::FirewallTable> table;
    // The ActionId of the rule with id k at index k - 1, so the packet path
    // names the action a matching rule carries (D-141) without holding the
    // rule list itself.
    std::vector<uint32_t> action_ids;
  };
  // A "<name>/rules" value, converted: the table to publish and the
  // FirewallArg it came from (read back as given).
  struct Staged {
    std::unique_ptr<const Published> published;
    bess::pb::FirewallArg arg;
  };

  // Validates and converts every rule of `arg`; nothing is kept on error.
  CommandResponse ParseRules(const bess::pb::FirewallArg &arg,
                             std::vector<bess::classifier::FirewallRule> *out)
      const;
  void Install(std::unique_ptr<const Published> published);

  // The packet's 5-tuple. False for a packet this table cannot judge: not IPv4
  // or IPv6, or a header it cannot read (a truncated one). A packet whose ports
  // are not in the frame - a non-first fragment, a transport behind more
  // extension headers than the walk follows - is judged with both ports 0,
  // which only a rule whose port range starts at 0 can match.
  static bool KeyOf(bess::PacketRef pkt, bess::classifier::FirewallKey *key);

  bess::rcu::RcuPtr<Published> rules_;
  // The rules as given, in order (control side): "<name>/rules" reads it
  // back.
  bess::pb::FirewallArg arg_;
  // "<name>/rules" (D-129): the whole ordered policy; setting it replaces the
  // table (an RCU swap: workers keep running).
  bess::framework::Singleton<bess::pb::FirewallArg, Staged> rules_resource_;
  // The packet's "action_id" (D-141): written for every packet the policy
  // passes, 0 when its rule names no action, so a module downstream selects
  // the action and a stale id cannot leak into a later lookup.
  int action_id_attr_ = -1;
  // The registered output name, for the log line that names it.
  std::string action_attr_name_;
  // Whether action_attr was configured, so the default name (a firewall with
  // nothing downstream) is quiet and a configured name with no reader is
  // reported. Kept at Init: arg_ is the resource's value and is replaced by a
  // rules update.
  bool action_attr_configured_ = false;
};

#endif  // BESS_MODULES_FIREWALL_H_
