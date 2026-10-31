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
        "entries", /*requires_paused_workers=*/false)};

class ArpResponder::EntriesResource::UpsertOp final : public dataplane::StagedOp {
 public:
  UpsertOp(EntriesResource &owner, uint32_t ip, Owned entry)
      : owner_(owner), ip_(ip), entry_(std::move(entry)) {}

  void Publish(dataplane::Retirer &retirer) noexcept override {
    // Claimed again now, by key: a sibling op of the same transaction may have
    // taken over the value-less claim Reserve made. Both phases run under the
    // engine's lock.
    const size_t slot = owner_.Claim(ip_);
    if (slot == kNoSlot) {
      // Reserve established room: unreachable. Nothing has seen the entry.
      retirer.Retire(std::move(entry_));
      return;
    }
    // The slot owns the entry from here.
    if (const Entry *old = owner_.Publish(slot, entry_.release()); old != nullptr) {
      retirer.Retire(owner_.Own(old));
    }
  }

 private:
  EntriesResource &owner_;
  uint32_t ip_;
  Owned entry_;
};

class ArpResponder::EntriesResource::EraseOp final : public dataplane::StagedOp {
 public:
  EraseOp(EntriesResource &owner, uint32_t ip) : owner_(owner), ip_(ip) {}

  void Publish(dataplane::Retirer &retirer) noexcept override {
    if (const Entry *old = owner_.Clear(ip_); old != nullptr) {
      retirer.Retire(owner_.Own(old));
    }
  }

 private:
  EntriesResource &owner_;
  uint32_t ip_;
};

ArpResponder::EntriesResource::EntriesResource(std::string name, size_t max_entries)
    : Resource(std::move(name)),
      slab_(bess::utils::ObjectSlab<Entry>::Create()),
      slots_(SlotCount(max_entries)),
      mask_(slots_.size() - 1) {}

ArpResponder::EntriesResource::~EntriesResource() {
  // Module teardown, after the workers stopped reading: the live entries go
  // with the table (ReleaseForTeardown tolerates them).
  for (const Slot &slot : slots_) {
    if (const Entry *entry = slot.value.load(std::memory_order_relaxed)) {
      slab_->Destroy(entry);
    }
  }
}

const ArpResponder::Entry *ArpResponder::EntriesResource::Lookup(
    uint32_t ip) const noexcept {
  if (ip == 0) {
    return nullptr;
  }
  size_t i = Hash(ip);
  for (size_t n = 0; n <= mask_; n++) {
    if (slots_[i].key.load(std::memory_order_acquire) == ip) {
      // A later transaction may reuse this slot for another address between
      // the two loads: an entry for a different address is not this key's.
      const Entry *entry = slots_[i].value.load(std::memory_order_acquire);
      if (entry != nullptr && entry->ip == ip) {
        return entry;
      }
    }
    if (slots_[i].passed.load(std::memory_order_acquire) == 0) {
      return nullptr;
    }
    i = (i + 1) & mask_;
  }
  return nullptr;
}

size_t ArpResponder::EntriesResource::Find(uint32_t ip) const noexcept {
  size_t i = Hash(ip);
  for (size_t n = 0; n <= mask_; n++) {
    if (slots_[i].key.load(std::memory_order_acquire) == ip) {
      return i;
    }
    if (slots_[i].passed.load(std::memory_order_acquire) == 0) {
      break;
    }
    i = (i + 1) & mask_;
  }
  return kNoSlot;
}

size_t ArpResponder::EntriesResource::Claim(uint32_t ip) noexcept {
  if (const size_t held = Find(ip); held != kNoSlot) {
    return held;
  }
  const size_t home = Hash(ip);
  size_t i = home;
  for (size_t n = 0; n <= mask_; n++) {
    const uint32_t key = slots_[i].key.load(std::memory_order_relaxed);
    if (key == 0 || slots_[i].value.load(std::memory_order_relaxed) == nullptr) {
      // Empty, or a claim no transaction published. Every slot before it
      // counts the pass; the counts precede the key's release store.
      if (key != 0) {
        Release(key, i);
      }
      for (size_t j = home; j != i; j = (j + 1) & mask_) {
        slots_[j].passed.fetch_add(1, std::memory_order_relaxed);
      }
      slots_[i].key.store(ip, std::memory_order_release);
      return i;
    }
    i = (i + 1) & mask_;
  }
  return kNoSlot;
}

const ArpResponder::Entry *ArpResponder::EntriesResource::Clear(uint32_t ip) noexcept {
  const size_t slot = Find(ip);
  if (slot == kNoSlot) {
    return nullptr;
  }
  const Entry *old = slots_[slot].value.exchange(nullptr, std::memory_order_acq_rel);
  if (old != nullptr) {
    live_--;
  }
  Release(ip, slot);
  return old;
}

void ArpResponder::EntriesResource::Release(uint32_t ip, size_t slot) noexcept {
  // Only `ip`'s own passes go: a live key's probe still counts in every slot
  // before its own.
  for (size_t j = Hash(ip); j != slot; j = (j + 1) & mask_) {
    slots_[j].passed.fetch_sub(1, std::memory_order_relaxed);
  }
  slots_[slot].key.store(0, std::memory_order_release);
}

const ArpResponder::Entry *ArpResponder::EntriesResource::Publish(
    size_t slot, const Entry *entry) noexcept {
  const Entry *old = slots_[slot].value.exchange(entry, std::memory_order_acq_rel);
  if (old == nullptr) {
    live_++;
  }
  return old;
}

bool ArpResponder::EntriesResource::Install(uint32_t ip, const Ethernet::Address &mac) {
  const size_t slot = Claim(ip);
  if (slot == kNoSlot) {
    return false;
  }
  if (const Entry *old = Publish(slot, slab_->Make(Entry{ip, mac}).release())) {
    slab_->Destroy(old);  // not published yet: no reader holds it
  }
  return true;
}

bool ArpResponder::EntriesResource::Contains(const dataplane::ResourceKey &key) const {
  uint32_t ip = 0;
  return dataplane::DecodeKey(key, &ip) && Lookup(ip) != nullptr;
}

std::expected<dataplane::Resource::Reservation, std::string>
ArpResponder::EntriesResource::Reserve(const dataplane::Op &op) {
  uint32_t ip = 0;
  if (!dataplane::DecodeKey(op.key, &ip)) {
    return std::unexpected("invalid IPv4 address key");
  }
  if (ip == 0) {
    return std::unexpected("0.0.0.0 cannot be answered for");
  }
  if (op.kind == dataplane::OpKind::kErase) {
    if (Lookup(ip) == nullptr) {
      return std::unexpected("not found");
    }
    Reservation erase{std::make_unique<EraseOp>(*this, ip), {}, {.retires = 1}};
    erase.existed = true;
    return erase;
  }
  const Ethernet::Address *mac = std::any_cast<Ethernet::Address>(&op.value);
  if (mac == nullptr) {
    return std::unexpected("wrong value type (want a MAC address)");
  }
  // The slot the entry will be published in, claimed now: the only step that
  // can find no room.
  const size_t slot = Claim(ip);
  if (slot == kNoSlot) {
    return std::unexpected("the entries table is full (" + std::to_string(mask_ + 1) +
                           " slots)");
  }
  const bool existed = slots_[slot].value.load(std::memory_order_acquire) != nullptr;
  // May add a slab chunk, admitted first (a refusal throws MemoryRefused).
  Owned entry = slab_->Make(Entry{ip, *mac});
  // Room for one replacement always: a sibling op may publish under this key
  // first.
  Reservation reservation{std::make_unique<UpsertOp>(*this, ip, std::move(entry)), {},
                          {.retires = 1}};
  reservation.existed = existed;
  return reservation;
}

CommandResponse ArpResponder::Init(const bess::pb::ArpResponderArg &arg) {
  const uint64_t max_entries = arg.max_entries() ? arg.max_entries() : kDefaultMaxEntries;
  // Validated before admitting (D-198).
  if (max_entries > kMaxEntriesLimit) {
    return CommandFailure(EINVAL, "max_entries %llu is above %zu",
                          static_cast<unsigned long long>(max_entries), kMaxEntriesLimit);
  }
  entries_ = init_context().memory().Create<EntriesResource>(
      "entries", name() + "/entries", static_cast<size_t>(max_entries));

  if (!arg.ip().empty() || !arg.mac_addr().empty()) {
    CommandResponse added = Add(arg);
    if (added.error().code() != 0) {
      entries_ = {};  // never registered: DeInit has nothing to release
      return added;
    }
  }

  binding_ = init_context().codecs().Bind(
      *entries_,
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
            entries_->ForEach([&](const Entry &entry) {
              bess::pb::ArpResponderEntryKey key;
              key.set_ip(bess::utils::ToIpv4Address(be32_t(entry.ip)));
              bess::pb::ArpResponderEntryValue value;
              value.set_mac_addr(entry.mac.ToString());
              visit(key, value);
            });
          }));
  if (auto registered = init_context().resources().Register(entries_.get());
      !registered) {
    binding_.Reset();
    entries_ = {};
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void ArpResponder::DeInit() {
  if (entries_.get() == nullptr) {
    return;
  }
  // The entries may still be live: the module leaves with them.
  const std::string name = entries_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  entries_ = {};
}

// The init argument's entry, validated before anything is installed.
CommandResponse ArpResponder::Add(const bess::pb::ArpResponderArg &arg) {
  be32_t ip_addr;
  Ethernet::Address mac;

  if (!arg.ip().length()) {
    return CommandFailure(EINVAL, "IP address is missing");
  }
  if (!bess::utils::ParseIpv4Address(arg.ip(), &ip_addr)) {
    return CommandFailure(EINVAL, "Invalid IP Address: %s", arg.ip().c_str());
  }
  if (ip_addr.value() == 0) {
    return CommandFailure(EINVAL, "0.0.0.0 cannot be answered for");
  }
  if (!mac.FromString(arg.mac_addr())) {
    return CommandFailure(EINVAL, "Invalid MAC Address: %s",
                          arg.mac_addr().c_str());
  }
  entries_->Install(ip_addr.value(), mac);
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
    const Entry *entry = entries_->Lookup(arp->target_ip.value());
    if (entry == nullptr) {
      // Did not find an ARP entry in cache, drop packet
      // TODO(galsagie) Optinally emit packet to next module here
      DropPacket(ctx, pkt);
      continue;
    }

    bess::PacketHandle handle = pkt.handle();
    const auto head = bess::packet::WritableHead(handle, bess::route::kArpFrameSize);
    if (unlikely(!head) ||
        !bess::route::AnswerArpRequest(
            std::span<uint8_t>(reinterpret_cast<uint8_t *>(head->data()), head->size()),
            entry->mac, be32_t(entry->ip))) {
      DropPacket(ctx, bess::PacketRef(handle));
      continue;
    }
    EmitPacket(ctx, bess::PacketRef(handle), 0);
  }
}

ADD_MODULE(ArpResponder, "arp_responder",
           "Respond to ARP requests and learns new MAC's")
