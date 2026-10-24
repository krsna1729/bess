// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "arp_responder.h"

#include <any>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "dataplane/transaction_engine.h"
#include "framework/resource_codec.h"
#include "packet_reshape.h"
#include "route/l3_packet.h"
#include "utils/arp.h"

namespace dataplane = bess::dataplane;
using bess::utils::Arp;

// The entries are the "<module>/entries" resource (D-129); the class has no
// actions.
const Commands ArpResponder::cmds = {};

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
    CommandResponse added = Add(arg);
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

// The init argument's entry, validated before anything is installed.
CommandResponse ArpResponder::Add(const bess::pb::ArpResponderArg &arg) {
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

// The request is read with the resolution library's ARP parse
// (route::ParseArp): an Ethernet frame whose first segment holds the 42-byte
// IPv4-over-Ethernet ARP message (EtherType, hardware and protocol types and
// lengths). Anything else -- not ARP, a runt, a chain split inside the
// message, another hardware or protocol type -- is dropped, as non-ARP frames
// always were. A request for an entry's address is rewritten in place into
// the reply (route::AnswerArpRequest, as Vif answers): a shared payload is
// copied first (D-153), and a frame whose copy fails is dropped. Replies,
// other opcodes and requests for other addresses are dropped.
void ArpResponder::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (bess::PacketRef pkt : batch->packets()) {
    // we should drop-or-emit each packet
    const std::optional<bess::route::ArpMessage> arp =
        bess::route::ParseArp(std::span<const uint8_t>(
            pkt.head_data<const uint8_t *>(), pkt.head_len()));
    // TODO(galsagie) When learn is added, learn SRC MAC from requests and
    // replies here.
    if (!arp || arp->opcode != Arp::Opcode::kRequest) {
      DropPacket(ctx, pkt);
      continue;
    }

    // Try to find target IP in cache, if exists convert request to reply
    auto it = entries_.find(arp->target_ip);
    if (it == entries_.end()) {
      // Did not find an ARP entry in cache, drop packet
      // TODO(galsagie) Optinally emit packet to next module here
      DropPacket(ctx, pkt);
      continue;
    }

    const struct arp_entry &entry = it->second;
    bess::PacketHandle handle = pkt.handle();
    const auto head = bess::packet::WritableHead(handle, bess::route::kArpFrameSize);
    if (unlikely(!head) ||
        !bess::route::AnswerArpRequest(
            std::span<uint8_t>(reinterpret_cast<uint8_t *>(head->data()), head->size()),
            entry.mac_addr, entry.ip_addr)) {
      DropPacket(ctx, bess::PacketRef(handle));
      continue;
    }
    EmitPacket(ctx, bess::PacketRef(handle), 0);
  }
}

ADD_MODULE(ArpResponder, "arp_responder",
           "Respond to ARP requests and learns new MAC's")
