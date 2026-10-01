// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ACTION_TABLE_H_
#define BESS_MODULES_ACTION_TABLE_H_

#include <memory>
#include <string>

#include "../dataplane/action_id.h"
#include "../dataplane/resource.h"
#include "../dataplane/slot_resource.h"
#include "../dataplane/slot_table.h"
#include "../meter/meter.h"
#include "../framework/resource_bindings.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../route/router.h"

// Per-session actions (K2's action table as a module, G1.2b as a resource
// provider).
//
// An ExactMatch rule in action mode names an action id; this module resolves
// it -- through the packet's `action_id` metadata -- to the meter the session
// is policed against and the next hop it forwards to, writes both to packet
// metadata, and passes the packet on. A packet whose action id resolves to
// nothing is dropped.
//
// The three tables are one transactional resource graph: an action references
// its meter (`meters`) and its next hop (`next_hops`), so one transaction can
// create the meter, the next hop, the route to it, the action and the rule
// that selects the action -- or none of them -- and the engine refuses an
// action whose meter or next hop is missing, or the removal of one still
// named. Both names are required configuration: they are what the engine
// declares, orders and checks. Neither module has to exist first: a
// declaration binds when its resource registers (D-021).
//
// An action's object is immutable and changed one id at a time (SlotTable,
// mode C); an erase keeps it readable for one removal-cascade stage, because
// a reader may still hold its id from a rule's old value.
class ActionTable final : public Module {
 public:
  static const gate_idx_t kNumOGates = 1;

  static const Commands cmds;

  using ActionId = bess::dataplane::ActionId;

  // What a session does: which meter polices it and where it forwards.
  struct Action {
    bess::meter::MeterId meter;       // 0: no meter (unmetered)
    bess::route::NextHopId next_hop;  // 0: no next hop (the Router drops)
  };

  ActionTable() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::ActionTableArg &arg);
  void DeInit() override;

  // Reports unreadable metadata attributes once per resume (the control
  // side's place for it, as in ExactMatch): the packet path only fails closed.
  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  // "<module>/actions", while the module exists.
  const std::string &actions_resource() const { return resource_->name(); }

  // The action an id names, or nullptr (one acquire load). The packet path
  // makes exactly this lookup; tests and introspection use it directly.
  const Action *LookupAction(ActionId id) const noexcept {
    return actions_->Lookup(id);
  }

 private:
  std::unique_ptr<bess::dataplane::SlotTable<ActionId, Action>> actions_;
  std::unique_ptr<bess::dataplane::SlotResource<ActionId, Action>> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)
  std::string meters_resource_;
  std::string next_hops_resource_;

  int action_id_attr_ = -1;
  int meter_id_attr_ = -1;
  int next_hop_id_attr_ = -1;
};

#endif  // BESS_MODULES_ACTION_TABLE_H_
