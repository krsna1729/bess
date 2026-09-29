// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_IPLOOKUP_H_
#define BESS_MODULES_IPLOOKUP_H_

#include <memory>
#include <string>
#include <tuple>

#include "../dataplane/strong_id.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../route/route_table.h"
#include "../utils/endian.h"

using bess::utils::be32_t;
using ParsedPrefix = std::tuple<int, std::string, be32_t>;

class IPLookup final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  IPLookup() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::IPLookupArg &arg);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::IPLookupCommandAddArg &arg);
  CommandResponse CommandDelete(const bess::pb::IPLookupCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

 private:
  // A route's value is an output gate (DROP_GATE included), stored directly:
  // this module's result *is* a gate, so there is no next-hop indirection.
  struct GateRouteTag;
  using GateRoute = bess::dataplane::StrongId<GateRouteTag, uint32_t>;

  ParsedPrefix ParseIpv4Prefix(const std::string &prefix, uint64_t prefix_len);

  // K7 route table: a live rte_lpm updated in place under the runtime's QSBR
  // (one rule insertion per add, not a rebuild), read lock-free per batch.
  // The /0 route is the default gate; with none, misses go to DROP_GATE.
  std::unique_ptr<bess::route::RouteTable<GateRoute>> routes_;
};

#endif  // BESS_MODULES_IPLOOKUP_H_
