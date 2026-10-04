// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_BRIDGE_H_
#define BESS_MODULES_BRIDGE_H_

#include <memory>
#include <string>
#include <vector>

#include "l2/fdb.h"
#include "l2/packed_mac_table.h"
#include "module.h"
#include "pb/module_msg.pb.h"

// Bridge: Ethernet L2 learning bridge with MAC aging and flooding, a thin
// adapter over the l2::Fdb library (M14, D-064). Learns source MAC -> ingress
// gate, forwards known unicast, floods unknown unicast and broadcast to every
// connected output gate except the ingress one, and drops hairpin traffic.
// Gate g is FDB interface g + 1 (DROP_GATE included, so a static entry can
// blackhole a MAC); the FDB never sees gates.
class Bridge final : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = MAX_GATES;
  static const Commands cmds;

  // Static entries beyond the learning limit (`size`).
  static constexpr size_t kStaticReserve = 1024;

  // One worker: the FDB is worker-owned (learning writes it on the packet path).
  // One bridge domain, so the one-word table (D-073: faster than MacTable in
  // every lookup and learn measured).
  using Fdb = bess::l2::BasicFdb<bess::l2::PackedMacTable<bess::dataplane::ExpiryHandle>>;

  Bridge() : Module() { max_allowed_workers_ = 1; }

  CommandResponse Init(const bess::pb::BridgeArg &arg);
  CommandResponse CommandAdd(const bess::pb::BridgeCommandAddArg &arg);
  CommandResponse CommandDelete(const bess::pb::BridgeCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::BridgeCommandClearArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override;

 private:
  void Flood(Context *ctx, bess::PacketRef pkt, gate_idx_t igate,
             const std::vector<gate_idx_t> &flood_gates);

  uint32_t max_entries_ = 1024;
  std::unique_ptr<Fdb> fdb_;
};

#endif  // BESS_MODULES_BRIDGE_H_
