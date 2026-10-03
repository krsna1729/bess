// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VIF_H_
#define BESS_MODULES_VIF_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/arp.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/ip.h"

// Vif: Virtual Interface multiplexer/demultiplexer.
// Manages multiple logical L2/L3 interfaces sharing a physical port or pipeline.
// Performs 802.1Q VLAN matching and stripping, sets vif_id and route_domain
// metadata, handles local ARP replies, and steers to per-VIF output gates.
class Vif final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;
  static const Commands cmds;

  struct Interface {
    uint32_t id = 0;
    uint16_t vlan = 0;  // 0: untagged
    bess::utils::Ethernet::Address mac_addr{};
    bess::utils::be32_t ip_addr{};
    uint32_t route_domain = 0;
    gate_idx_t gate = 0;
  };

  Vif() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::VifArg &arg);
  CommandResponse CommandAdd(const bess::pb::VifCommandAddArg &arg);
  CommandResponse CommandDelete(const bess::pb::VifCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::VifCommandClearArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override;

 private:
  void RebuildTables();
  const Interface *Lookup(uint16_t vlan,
                          const bess::utils::Ethernet::Address &dmac) const;
  const Interface *LookupByIp(uint16_t vlan, bess::utils::be32_t ip) const;

  std::map<uint32_t, Interface> interfaces_by_id_;
  // Key: (vlan, mac as uint64)
  std::map<std::pair<uint16_t, uint64_t>, const Interface *> mac_table_;
  // Key: (vlan, ip) for ARP responder
  std::map<std::pair<uint16_t, uint32_t>, const Interface *> arp_table_;

  gate_idx_t default_gate_ = DROP_GATE;
  int vif_id_attr_ = -1;
  int route_domain_attr_ = -1;
};

#endif  // BESS_MODULES_VIF_H_
