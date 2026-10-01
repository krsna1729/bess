// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ROUTER_H_
#define BESS_MODULES_ROUTER_H_

#include <memory>
#include <string>

#include "../framework/resource_bindings.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../route/router.h"

// K7's route table and next hops as a module, enrolled in the transaction
// engine (D-023).
//
// Packet path: the next-hop id arrives in packet metadata (a `next_hop_id`
// be32 attribute an ActionTable writes), the next hop is resolved -- one
// acquire load per id -- and the packet leaves on the gate that hop's egress
// names (in a module graph, the path to the egress port). A packet whose id
// names no next hop is dropped, and so is one whose neighbor is not resolved:
// what to do toward an unresolved neighbor (queue, punt, drop) is the
// application's, and dropping is the choice that never forwards on a guess.
// Rewriting the next hop's L2 addresses is a separate module's job (Rewrite
// reads them from metadata); this module decides the egress.
//
// Control path: `<module>/next_hops` and `<module>/routes` are resources, so
// one transaction can create a session's next hop, the route to it, the
// action that names it and the rule that selects the action. An enrolled
// router has one writer, the engine: the direct setters refuse with kEnrolled,
// and the engine's ledger replaces the router's own reference counts. New
// routes are placed during prepare with the value their addresses already
// resolve to, so rte_lpm capacity is settled before anything is visible.
class Router final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  Router() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::RouterArg &arg);
  void DeInit() override;

  // Reports an unreadable 'next_hop_id' attribute once per resume (the
  // control side's place for it, as in ExactMatch): the packet path only
  // fails closed.
  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  // The enrolled router, for tests and for the module's own introspection.
  const bess::route::Router *router() const { return router_.get(); }

 private:
  std::unique_ptr<bess::route::Router> router_;
  // After router_, which owns the resources they bind (D-044).
  bess::framework::ResourceBinding next_hops_binding_;
  bess::framework::ResourceBinding routes_binding_;
  int next_hop_id_attr_ = -1;
};

#endif  // BESS_MODULES_ROUTER_H_
