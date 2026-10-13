// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "arp_responder.h"

#include <any>
#include <expected>
#include <span>
#include <string>
#include <utility>

#include "dataplane/transaction_engine.h"
#include "framework/resource_codec.h"
#include "utils/arp.h"

namespace dataplane = bess::dataplane;
using bess::utils::Arp;
using bess::utils::be16_t;

// LEGACY_ONLY (D-129): the entries are the "<module>/entries" resource.
const Commands ArpResponder::cmds = {
    {"add", "ArpResponderArg", MODULE_CMD_FUNC(&ArpResponder::CommandAdd),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY}};

const ResourceTables ArpResponder::resource_tables = {
    ResourceTable::Map<bess::pb::ArpResponderEntryKey,
                       bess::pb::ArpResponderEntryValue>(
        "entries", /*requires_paused_workers=*/true)};

// "<module>/entries": key EncodeKey(the IPv4 address, host order), value the
// MAC (Ethernet::Address). Workers read the map without synchronization, so
// this runs with them paused (the codec says so; Reserve refuses otherwise
// too). Publish allocates nothing: an upsert's map node is built in Reserve,
// and an erased node leaves with its operation.
class ArpResponder::EntriesResource final : public dataplane::Resource {
 public:
  explicit EntriesResource(ArpResponder &owner)
      : Resource(owner.name() + "/entries"), owner_(owner) {}

  size_t LiveCount() const override { return owner_.entries_.size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    uint32_t ip;
    return dataplane::DecodeKey(key, &ip) && owner_.entries_.contains(be32_t(ip));
  }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    if (owner_.HasRunningWorker()) {
      return std::unexpected(
          "a worker runs the module (entries require paused workers)");
    }
    uint32_t host_ip;
    if (!dataplane::DecodeKey(op.key, &host_ip)) {
      return std::unexpected("invalid IPv4 address key");
    }
    const be32_t ip(host_ip);
    const bool existed = owner_.entries_.contains(ip);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!existed) {
        return std::unexpected("not found");
      }
      Reservation reservation{std::make_unique<EraseOp>(owner_, ip), {}, {}};
      reservation.existed = true;
      return reservation;
    }
    const Ethernet::Address *mac = std::any_cast<Ethernet::Address>(&op.value);
    if (mac == nullptr) {
      return std::unexpected("wrong value type (want a MAC address)");
    }
    arp_entry entry;
    entry.mac_addr = *mac;
    entry.ip_addr = ip;
    entry.time = 0;
    Map node_source;
    node_source.emplace(ip, entry);
    Reservation reservation{
        std::make_unique<UpsertOp>(owner_,
                                   node_source.extract(node_source.begin())),
        {}, {}};
    reservation.existed = existed;
    return reservation;
  }

 private:
  using Map = std::map<be32_t, arp_entry>;
  using Node = Map::node_type;

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(ArpResponder &owner, Node node)
        : owner_(owner), node_(std::move(node)) {}
    void Publish(dataplane::Retirer &) noexcept override {
      auto it = owner_.entries_.find(node_.key());
      if (it != owner_.entries_.end()) {
        it->second = node_.mapped();
      } else {
        owner_.entries_.insert(std::move(node_));
      }
    }

   private:
    ArpResponder &owner_;
    Node node_;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(ArpResponder &owner, be32_t ip) : owner_(owner), ip_(ip) {}
    void Publish(dataplane::Retirer &) noexcept override {
      erased_ = owner_.entries_.extract(ip_);  // freed with this operation
    }

   private:
    ArpResponder &owner_;
    be32_t ip_;
    Node erased_;
  };

  ArpResponder &owner_;
};

CommandResponse ArpResponder::Init(const bess::pb::ArpResponderArg &arg) {
  if (!arg.ip().empty() || !arg.mac_addr().empty()) {
    CommandResponse added = CommandAdd(arg);
    if (added.error().code() != 0) {
      return added;
    }
  }

  resource_ = std::make_unique<EntriesResource>(*this);
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<
          bess::pb::ArpResponderEntryKey, bess::pb::ArpResponderEntryValue>>(
          [](const bess::pb::ArpResponderEntryKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            be32_t ip;
            if (key.ip().empty()) {
              return std::unexpected("IP address is missing");
            }
            if (!bess::utils::ParseIpv4Address(key.ip(), &ip)) {
              return std::unexpected("Invalid IP Address: " + key.ip());
            }
            return dataplane::EncodeKey(ip.value());
          },
          [](const bess::pb::ArpResponderEntryValue &value)
              -> std::expected<std::any, std::string> {
            Ethernet::Address mac;
            if (!mac.FromString(value.mac_addr())) {
              return std::unexpected("Invalid MAC Address: " + value.mac_addr());
            }
            return std::any(mac);
          },
          // Read back as a client sends an entry: the address in dotted
          // decimal, the MAC in lower case.
          [this](const auto &visit) {
            for (const auto &[ip, entry] : entries_) {
              bess::pb::ArpResponderEntryKey key;
              key.set_ip(bess::utils::ToIpv4Address(ip));
              bess::pb::ArpResponderEntryValue value;
              value.set_mac_addr(entry.mac_addr.ToString());
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

void ArpResponder::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // The entries may still be live: the module leaves with them.
  const std::string name = resource_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  resource_.reset();
}

CommandResponse ArpResponder::CommandAdd(const bess::pb::ArpResponderArg &arg) {
  be32_t ip_addr;
  arp_entry entry;

  if (!arg.ip().length()) {
    return CommandFailure(EINVAL, "IP address is missing");
  }
  if (!bess::utils::ParseIpv4Address(arg.ip(), &ip_addr)) {
    return CommandFailure(EINVAL, "Invalid IP Address: %s", arg.ip().c_str());
  }

  if (!entry.mac_addr.FromString(arg.mac_addr())) {
    return CommandFailure(EINVAL, "Invalid MAC Address: %s",
                          arg.mac_addr().c_str());
  }

  entry.ip_addr = ip_addr;
  entries_[ip_addr] = entry;
  return CommandSuccess();
}

void ArpResponder::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (bess::PacketRef pkt : batch->packets()) {
    // we should drop-or-emit each packet
    Ethernet *eth = pkt.head_data<Ethernet *>();
    if (eth->ether_type != be16_t(Ethernet::Type::kArp)) {
      // Currently drop all non ARP packets
      DropPacket(ctx, pkt);
      continue;
    }

    Arp *arp = reinterpret_cast<Arp *>(eth + 1);
    if (arp->opcode == be16_t(Arp::Opcode::kRequest)) {
      // TODO(galsagie) When learn is added, learn SRC MAC here

      // Try to find target IP in cache, if exists convert request to reply
      auto it = entries_.find(arp->target_ip_addr);
      if (it != entries_.end()) {
        const struct arp_entry &entry = it->second;
        arp->opcode = be16_t(Arp::Opcode::kReply);

        eth->dst_addr = eth->src_addr;
        eth->src_addr = entry.mac_addr;

        arp->target_hw_addr = arp->sender_hw_addr;
        arp->sender_hw_addr = entry.mac_addr;

        arp->target_ip_addr = arp->sender_ip_addr;
        arp->sender_ip_addr = entry.ip_addr;
        EmitPacket(ctx, pkt, 0);
      } else {
        // Did not find an ARP entry in cache, drop packet
        // TODO(galsagie) Optinally emit packet to next module here
        DropPacket(ctx, pkt);
      }
    } else if (arp->opcode == be16_t(Arp::Opcode::kReply)) {
      // TODO(galsagie) When learn is added, learn SRC MAC here
      DropPacket(ctx, pkt);
    } else {
      // TODO(galsagie) Other opcodes are not handled yet.
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(ArpResponder, "arp_responder",
           "Respond to ARP requests and learns new MAC's")
