// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VIF_H_
#define BESS_MODULES_VIF_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/resource.h"
#include "framework/resource_bindings.h"
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
//
// Control path (D-129): the interfaces are the "<module>/interfaces"
// resource (key VifInterfaceKey, value VifInterfaceValue). Workers read the
// tables without synchronization, so the resource requires paused workers.
class Vif final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;
  static const Commands cmds;
  static const ResourceTables resource_tables;

  struct Interface {
    uint32_t id = 0;
    uint16_t vlan = 0;  // 0: untagged
    bess::utils::Ethernet::Address mac_addr{};
    bess::utils::be32_t ip_addr{};
    bess::utils::Ipv6Address ipv6_addr{};
    uint32_t route_domain = 0;
    gate_idx_t gate = 0;
  };

  Vif() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::VifArg &arg);
  void DeInit() override;

  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override;

  // The module's `interfaces` resource.
  class InterfacesResource;

 private:
  friend class InterfacesResource;

  // The init argument's interfaces, validated before anything is installed.
  CommandResponse Add(const bess::pb::VifCommandAddArg &arg);

  using InterfaceMap = std::map<uint32_t, Interface>;
  // Key: (vlan, mac as uint64)
  using MacIndex = std::map<std::pair<uint16_t, uint64_t>, const Interface *>;
  // Key: (vlan, ip) for ARP responder
  using ArpIndex = std::map<std::pair<uint16_t, uint32_t>, const Interface *>;
  // Key: (vlan, IPv6 address) for the ND responder
  using NdIndex =
      std::map<std::pair<uint16_t, bess::utils::Ipv6Address>, const Interface *>;

  // A transaction's edited copy of the interfaces with its indexes, built in
  // Reserve and swapped in by the first publish (no allocation there).
  struct Staged {
    InterfaceMap interfaces_by_id;
    MacIndex mac_table;
    ArpIndex arp_table;
    NdIndex nd_table;
  };

  // The indexes of `interfaces` (pointing into its nodes).
  static void BuildIndexes(const InterfaceMap &interfaces, MacIndex *mac_table,
                           ArpIndex *arp_table, NdIndex *nd_table);
  void RebuildTables();
  const Interface *Lookup(uint16_t vlan,
                          const bess::utils::Ethernet::Address &dmac) const;
  const Interface *LookupByIp(uint16_t vlan, bess::utils::be32_t ip) const;
  const Interface *LookupByIpv6(uint16_t vlan,
                                const bess::utils::Ipv6Address &ip) const;

  // The transaction's copy (made by its first reservation).
  Staged &BeginTransaction();
  // Swaps the copy in, once per transaction.
  void PublishStaged() noexcept;
  // A transaction ended (published or aborted): the copy (now the replaced
  // tables, if it published) goes.
  void EndTransaction() noexcept;

  InterfaceMap interfaces_by_id_;
  MacIndex mac_table_;
  ArpIndex arp_table_;
  NdIndex nd_table_;
  std::unique_ptr<Staged> staged_;
  bool published_staged_ = false;

  gate_idx_t default_gate_ = DROP_GATE;
  int vif_id_attr_ = -1;
  int route_domain_attr_ = -1;
  // The registered names, for the log lines that name them.
  std::string vif_id_attr_name_;
  std::string domain_attr_name_;
  // Whether each name was configured, so a default (a graph that consumes
  // neither) is quiet and a configured name with no reader is reported.
  bool vif_id_attr_configured_ = false;
  bool domain_attr_configured_ = false;

  std::unique_ptr<bess::dataplane::Resource> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)
};

#endif  // BESS_MODULES_VIF_H_
