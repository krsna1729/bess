// SPDX-License-Identifier: BSD-3-Clause

#include "vif.h"

#include <any>
#include <cstring>
#include <expected>
#include <span>
#include <string>

#include "arch/vlan.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_codec.h"
#include "utils/format.h"

namespace {

namespace dataplane = bess::dataplane;
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

// LEGACY_ONLY (D-129): the interfaces are the "<module>/interfaces" resource.
const Commands Vif::cmds = {
    {"add", "VifCommandAddArg", MODULE_CMD_FUNC(&Vif::CommandAdd),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY},
    {"delete", "VifCommandDeleteArg", MODULE_CMD_FUNC(&Vif::CommandDelete),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY},
    {"clear", "VifCommandClearArg", MODULE_CMD_FUNC(&Vif::CommandClear),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY},
};

const ResourceTables Vif::resource_tables = {
    ResourceTable::Map<bess::pb::VifInterfaceKey, bess::pb::VifInterfaceValue>(
        "interfaces", /*requires_paused_workers=*/true)};

// "<module>/interfaces": key EncodeKey(id), value an Interface (its id
// unset). Workers read the tables without synchronization, so this runs with
// them paused (the codec says so; Reserve refuses otherwise too). Every
// reservation edits the transaction's copy of the tables and rebuilds its
// indexes; the first publish swaps the copy in, which allocates nothing.
class Vif::InterfacesResource final : public dataplane::Resource {
 public:
  explicit InterfacesResource(Vif &owner)
      : Resource(owner.name() + "/interfaces"), owner_(owner) {}

  size_t LiveCount() const override { return owner_.interfaces_by_id_.size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    uint32_t id;
    return dataplane::DecodeKey(key, &id) &&
           owner_.interfaces_by_id_.contains(id);
  }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    if (owner_.HasRunningWorker()) {
      return std::unexpected(
          "a worker runs the module (interfaces require paused workers)");
    }
    uint32_t id;
    if (!dataplane::DecodeKey(op.key, &id) || id == 0) {
      return std::unexpected("invalid interface id");
    }
    const bool existed = owner_.interfaces_by_id_.contains(id);
    Interface iface;
    if (op.kind == dataplane::OpKind::kErase) {
      if (!existed) {
        return std::unexpected("not found");
      }
    } else {
      const Interface *value = std::any_cast<Interface>(&op.value);
      if (value == nullptr) {
        return std::unexpected("wrong value type (want a Vif interface)");
      }
      iface = *value;
      iface.id = id;
    }

    Staged &staged = owner_.BeginTransaction();
    if (op.kind == dataplane::OpKind::kErase) {
      staged.interfaces_by_id.erase(id);
    } else {
      staged.interfaces_by_id[id] = iface;
    }
    BuildIndexes(staged.interfaces_by_id, &staged.mac_table, &staged.arp_table);
    Reservation reservation{std::make_unique<PublishOp>(owner_), {}, {}};
    reservation.existed = existed;
    return reservation;
  }

  void EndTransaction() noexcept override { owner_.EndTransaction(); }

 private:
  class PublishOp final : public dataplane::StagedOp {
   public:
    explicit PublishOp(Vif &owner) : owner_(owner) {}
    void Publish(dataplane::Retirer &) noexcept override {
      owner_.PublishStaged();
    }

   private:
    Vif &owner_;
  };

  Vif &owner_;
};

void Vif::BuildIndexes(const InterfaceMap &interfaces, MacIndex *mac_table,
                       ArpIndex *arp_table) {
  mac_table->clear();
  arp_table->clear();
  for (const auto &[id, iface] : interfaces) {
    (*mac_table)[{iface.vlan, MacToUint64(iface.mac_addr)}] = &iface;
    if (iface.ip_addr.value() != 0) {
      (*arp_table)[{iface.vlan, iface.ip_addr.value()}] = &iface;
    }
  }
}

void Vif::RebuildTables() {
  BuildIndexes(interfaces_by_id_, &mac_table_, &arp_table_);
}

Vif::Staged &Vif::BeginTransaction() {
  if (staged_ == nullptr) {
    staged_ = std::make_unique<Staged>();
    staged_->interfaces_by_id = interfaces_by_id_;
    published_staged_ = false;
  }
  return *staged_;
}

void Vif::PublishStaged() noexcept {
  // Every reservation of the transaction edited the same copy: the first
  // publish makes all of it live (map swaps keep the indexes' node pointers
  // valid), the rest find it done.
  if (!published_staged_) {
    interfaces_by_id_.swap(staged_->interfaces_by_id);
    mac_table_.swap(staged_->mac_table);
    arp_table_.swap(staged_->arp_table);
    published_staged_ = true;
  }
}

void Vif::EndTransaction() noexcept {
  staged_.reset();
  published_staged_ = false;
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

  resource_ = std::make_unique<InterfacesResource>(*this);
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::VifInterfaceKey,
                                                   bess::pb::VifInterfaceValue>>(
          [](const bess::pb::VifInterfaceKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("interface 'id' must be non-zero");
            }
            return dataplane::EncodeKey(key.id());
          },
          [](const bess::pb::VifInterfaceValue &value)
              -> std::expected<std::any, std::string> {
            if (!bess::IsValidGateValue(value.gate())) {
              return std::unexpected("invalid output gate: " +
                                     std::to_string(value.gate()));
            }
            // add masks the VLAN; a value must read back as written.
            if (value.vlan() > 0x0FFF) {
              return std::unexpected("invalid VLAN: " +
                                     std::to_string(value.vlan()));
            }
            Interface iface;
            iface.vlan = static_cast<uint16_t>(value.vlan());
            iface.route_domain = value.route_domain();
            iface.gate = static_cast<gate_idx_t>(value.gate());
            if (!value.mac_addr().empty() &&
                !iface.mac_addr.FromString(value.mac_addr())) {
              return std::unexpected("invalid MAC address: '" +
                                     value.mac_addr() + "'");
            }
            if (!value.ip_addr().empty() &&
                !bess::utils::ParseIpv4Address(value.ip_addr(), &iface.ip_addr)) {
              return std::unexpected("invalid IPv4 address: '" +
                                     value.ip_addr() + "'");
            }
            return std::any(iface);
          },
          // Read back as a client sends an interface: the MAC in lower case
          // and the address in dotted decimal, each omitted when zero (what
          // an omitted one means).
          [this](const auto &visit) {
            for (const auto &[id, iface] : interfaces_by_id_) {
              bess::pb::VifInterfaceKey key;
              key.set_id(id);
              bess::pb::VifInterfaceValue value;
              value.set_vlan(iface.vlan);
              if (MacToUint64(iface.mac_addr) != 0) {
                value.set_mac_addr(iface.mac_addr.ToString());
              }
              if (iface.ip_addr.value() != 0) {
                value.set_ip_addr(bess::utils::ToIpv4Address(iface.ip_addr));
              }
              value.set_route_domain(iface.route_domain);
              value.set_gate(iface.gate);
              visit(key, value);
            }
          },
          [this] { return HasRunningWorker(); }));
  if (auto registered = init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Vif::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // The interfaces may still be live: the module leaves with them.
  const std::string name = resource_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  resource_.reset();
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
  const auto vif_offset = attr_offset(vif_id_attr_);
  const auto domain_offset = attr_offset(route_domain_attr_);
  const bool write_metadata = bess::metadata::IsValidOffset(vif_offset) &&
                              bess::metadata::IsValidOffset(domain_offset);

  for (bess::PacketRef pkt : batch->packets()) {
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
