// SPDX-License-Identifier: BSD-3-Clause

#include "vif.h"

#include <cstring>

#include "arch/vlan.h"
#include "utils/format.h"

namespace {

using bess::utils::Arp;
using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::Ethernet;
using bess::utils::Vlan;

uint64_t MacToUint64(const Ethernet::Address &addr) {
  uint64_t v = 0;
  std::memcpy(&v, addr.bytes, Ethernet::Address::kSize);
  return v;
}

}  // namespace

const Commands Vif::cmds = {
    {"add", "VifCommandAddArg", MODULE_CMD_FUNC(&Vif::CommandAdd),
     Command::THREAD_UNSAFE},
    {"delete", "VifCommandDeleteArg", MODULE_CMD_FUNC(&Vif::CommandDelete),
     Command::THREAD_UNSAFE},
    {"clear", "VifCommandClearArg", MODULE_CMD_FUNC(&Vif::CommandClear),
     Command::THREAD_UNSAFE},
};

void Vif::RebuildTables() {
  mac_table_.clear();
  arp_table_.clear();
  for (const auto &[id, iface] : interfaces_by_id_) {
    mac_table_[{iface.vlan, MacToUint64(iface.mac_addr)}] = &iface;
    if (iface.ip_addr.value() != 0) {
      arp_table_[{iface.vlan, iface.ip_addr.value()}] = &iface;
    }
  }
}

const Vif::Interface *Vif::Lookup(
    uint16_t vlan, const bess::utils::Ethernet::Address &dmac) const {
  auto it = mac_table_.find({vlan, MacToUint64(dmac)});
  if (it != mac_table_.end()) {
    return it->second;
  }
  return nullptr;
}

const Vif::Interface *Vif::LookupByIp(uint16_t vlan,
                                      bess::utils::be32_t ip) const {
  auto it = arp_table_.find({vlan, ip.value()});
  if (it != arp_table_.end()) {
    return it->second;
  }
  return nullptr;
}

CommandResponse Vif::Init(const bess::pb::VifArg &arg) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  vif_id_attr_ = AddMetadataAttr("vif_id", sizeof(be32_t), AccessMode::kWrite);
  route_domain_attr_ =
      AddMetadataAttr("route_domain", sizeof(be32_t), AccessMode::kWrite);
  if (vif_id_attr_ < 0 || route_domain_attr_ < 0) {
    return CommandFailure(EINVAL, "add_metadata_attr() failed");
  }

  default_gate_ = static_cast<gate_idx_t>(arg.default_gate());

  for (const auto &iface_pb : arg.interfaces()) {
    bess::pb::VifCommandAddArg add_arg;
    add_arg.set_id(iface_pb.id());
    add_arg.set_vlan(iface_pb.vlan());
    add_arg.set_mac_addr(iface_pb.mac_addr());
    add_arg.set_ip_addr(iface_pb.ip_addr());
    add_arg.set_route_domain(iface_pb.route_domain());
    add_arg.set_gate(iface_pb.gate());

    CommandResponse ret = CommandAdd(add_arg);
    if (ret.error().code() != 0) {
      return ret;
    }
  }

  return CommandSuccess();
}

CommandResponse Vif::CommandAdd(const bess::pb::VifCommandAddArg &arg) {
  if (arg.id() == 0) {
    return CommandFailure(EINVAL, "interface 'id' must be non-zero");
  }
  if (!bess::IsValidGateValue(arg.gate())) {
    return CommandFailure(EINVAL, "invalid output gate: %u", arg.gate());
  }

  Interface iface;
  iface.id = arg.id();
  iface.vlan = static_cast<uint16_t>(arg.vlan() & 0x0FFF);
  iface.route_domain = arg.route_domain();
  iface.gate = static_cast<gate_idx_t>(arg.gate());

  if (!arg.mac_addr().empty()) {
    if (!iface.mac_addr.FromString(arg.mac_addr())) {
      return CommandFailure(EINVAL, "invalid MAC address: '%s'",
                            arg.mac_addr().c_str());
    }
  }

  if (!arg.ip_addr().empty()) {
    if (!bess::utils::ParseIpv4Address(arg.ip_addr(), &iface.ip_addr)) {
      return CommandFailure(EINVAL, "invalid IPv4 address: '%s'",
                            arg.ip_addr().c_str());
    }
  }

  interfaces_by_id_[iface.id] = iface;
  RebuildTables();
  return CommandSuccess();
}

CommandResponse Vif::CommandDelete(const bess::pb::VifCommandDeleteArg &arg) {
  if (interfaces_by_id_.erase(arg.id()) == 0) {
    return CommandFailure(ENOENT, "interface id %u not found", arg.id());
  }
  RebuildTables();
  return CommandSuccess();
}

CommandResponse Vif::CommandClear(const bess::pb::VifCommandClearArg &) {
  interfaces_by_id_.clear();
  mac_table_.clear();
  arp_table_.clear();
  return CommandSuccess();
}

void Vif::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const auto vif_offset = attr_offset(vif_id_attr_);
  const auto domain_offset = attr_offset(route_domain_attr_);
  const bool write_metadata = bess::metadata::IsValidOffset(vif_offset) &&
                              bess::metadata::IsValidOffset(domain_offset);

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    char *head = pkt.head_data<char *>();
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      EmitPacket(ctx, pkt, default_gate_);
      continue;
    }

    uint16_t vlan = 0;
    // Check for an 802.1Q / 802.1ad VLAN tag.
    const be16_t tpid = reinterpret_cast<const Ethernet *>(head)->ether_type;

    bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                  (tpid == be16_t(Ethernet::Type::kQinQ));

    if (tagged && pkt.head_len() >= sizeof(Ethernet) + 4) {
      const be16_t tci =
          reinterpret_cast<const Vlan *>(head + sizeof(Ethernet))->tci;
      vlan = tci.value() & 0x0FFF;

      // Strip the VLAN tag in place.
      if (pkt.adj(4)) {
        bess::arch::RemoveVlanTag(head);
      }
    }

    Ethernet *eth = pkt.head_data<Ethernet *>();

    // Handle ARP Requests for local VIF addresses
    if (eth->ether_type == be16_t(Ethernet::Type::kArp) &&
        pkt.head_len() >= sizeof(Ethernet) + sizeof(Arp)) {
      Arp *arp = reinterpret_cast<Arp *>(eth + 1);
      if (arp->opcode == be16_t(Arp::Opcode::kRequest)) {
        const Interface *iface = LookupByIp(vlan, arp->target_ip_addr);
        if (iface != nullptr) {
          // Generate ARP Reply in-place
          arp->opcode = be16_t(Arp::Opcode::kReply);
          eth->dst_addr = eth->src_addr;
          eth->src_addr = iface->mac_addr;

          arp->target_hw_addr = arp->sender_hw_addr;
          arp->sender_hw_addr = iface->mac_addr;
          arp->target_ip_addr = arp->sender_ip_addr;
          arp->sender_ip_addr = iface->ip_addr;

          if (write_metadata) {
            _set_attr_with_offset<be32_t>(vif_offset, pkt, be32_t(iface->id));
            _set_attr_with_offset<be32_t>(domain_offset, pkt,
                                          be32_t(iface->route_domain));
          }
          EmitPacket(ctx, pkt, iface->gate);
          continue;
        }
      }
    }

    // Lookup destination VIF by (vlan, dmac)
    const Interface *iface = Lookup(vlan, eth->dst_addr);
    if (iface != nullptr) {
      if (write_metadata) {
        _set_attr_with_offset<be32_t>(vif_offset, pkt, be32_t(iface->id));
        _set_attr_with_offset<be32_t>(domain_offset, pkt,
                                      be32_t(iface->route_domain));
      }
      EmitPacket(ctx, pkt, iface->gate);
    } else {
      EmitPacket(ctx, pkt, default_gate_);
    }
  }
}

std::string Vif::GetDesc() const {
  return bess::utils::Format("%zu interfaces", interfaces_by_id_.size());
}

ADD_MODULE(Vif, "vif",
           "Virtual interface multiplexer, VLAN tag stripper, and local ARP "
           "responder")
