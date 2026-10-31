// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ARP_RESPONDER_H_
#define BESS_MODULES_ARP_RESPONDER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "dataplane/resource.h"
#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/dpdk_slab.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/ip.h"

using bess::utils::Ethernet;
using bess::utils::be32_t;

// ARP Responder module
// Answer ARP requests from an internal configurable cache
// Currently drops non ARP packets
//
// Control path (D-129): the cache is the "<module>/entries" resource (key
// ArpResponderEntryKey, value ArpResponderEntryValue), applied live: workers
// read it lock-free while a transaction changes it (MA-B4).
class ArpResponder final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 1;

  static const Commands cmds;
  static const ResourceTables resource_tables;
  // The packet path only reads the control-written ARP cache.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  // Entries the table holds, by default and at most (`max_entries`).
  static constexpr size_t kDefaultMaxEntries = 1024;
  static constexpr size_t kMaxEntriesLimit = size_t{1} << 20;

  // One answer: the address and the Ethernet address its replies carry.
  struct Entry {
    uint32_t ip = 0;  // host order
    Ethernet::Address mac;
  };

  // "<module>/entries": IPv4 (host order) -> MAC, live. An open-addressed
  // table of atomic slots sized from max_entries at Init and never grown: a
  // worker's lookup is acquire loads, a transaction claims a slot and
  // publishes its entry under the engine's lock, and a replaced entry is
  // retired, not freed. An erase empties its slot and moves nothing: each slot
  // counts the claimed keys whose probe went past it, a probe stops at the
  // first slot none went past, and an erase takes its passes back (no
  // tombstones, as flow/flow_index.h). Memory (D-198): the slot array is
  // Bytes(); the entries come from an ObjectSlab whose chunks are admitted as
  // growth before Reserve allocates them.
  class EntriesResource;

  CommandResponse Init(const bess::pb::ArpResponderArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  // The init argument's entry, validated before anything is installed.
  CommandResponse Add(const bess::pb::ArpResponderArg &arg);

  bess::framework::Sized<EntriesResource> entries_;
  bess::framework::ResourceBinding binding_;  // after entries_ (D-044)
};

class ArpResponder::EntriesResource final : public bess::dataplane::Resource {
 public:
  EntriesResource(std::string name, size_t max_entries);
  ~EntriesResource() override;

  static size_t Bytes(const std::string &name, size_t max_entries) noexcept {
    return GlibcBytes(name, max_entries) + bess::utils::ObjectSlab<Entry>::Bytes();
  }
  static size_t GlibcBytes(const std::string & /*name*/, size_t max_entries) noexcept {
    return SlotCount(max_entries) * sizeof(Slot);
  }
  // The slot array plus the entry chunks made so far.
  size_t bytes() const noexcept { return slots_.size() * sizeof(Slot) + slab_->bytes(); }
  // The admission each new entry chunk asks first (D-198).
  void set_memory_growth(bess::utils::MemoryGrowth *growth) noexcept {
    slab_->set_memory_growth(growth);
  }

  // The entry for an address, or nullptr: the packet path's whole lookup.
  const Entry *Lookup(uint32_t ip) const noexcept;

  // Installs the init argument's entry (Init, before registration).
  bool Install(uint32_t ip, const Ethernet::Address &mac);

  size_t LiveCount() const override { return live_; }
  bool Contains(const bess::dataplane::ResourceKey &key) const override;
  std::expected<Reservation, std::string> Reserve(const bess::dataplane::Op &op) override;
  // A slot a Reserve claimed keeps its claim (without a value, if the
  // transaction never publishes) until a later claim takes it over.
  void EndTransaction() noexcept override {}

  // Every live entry, control side: what "<module>/entries" reads back.
  template <typename Fn>
  void ForEach(Fn &&visit) const {
    for (const Slot &slot : slots_) {
      const uint32_t key = slot.key.load(std::memory_order_acquire);
      const Entry *value = slot.value.load(std::memory_order_acquire);
      if (key != 0 && value != nullptr) {
        visit(*value);
      }
    }
  }

 private:
  static constexpr size_t kNoSlot = ~size_t{0};
  using Owned = bess::utils::SlabUnique<const Entry>;
  class UpsertOp;
  class EraseOp;

  // The smallest power of two at least max(2, 2 x max_entries).
  static size_t SlotCount(size_t max_entries) noexcept {
    size_t slots = 2;
    while (slots < max_entries * 2) {
      slots <<= 1;
    }
    return slots;
  }

  struct Slot {
    // 0 is never an address this table holds (Reserve refuses it).
    std::atomic<uint32_t> key{0};
    // The claimed keys whose probe went past this slot to a later one.
    std::atomic<uint32_t> passed{0};
    std::atomic<const Entry *> value{nullptr};
  };

  size_t Hash(uint32_t ip) const noexcept {
    // Addresses are structured (a prefix): mix before masking.
    return (static_cast<size_t>(ip) * 2654435761u) & mask_;
  }
  // The slot holding `ip`, or kNoSlot. Control side.
  size_t Find(uint32_t ip) const noexcept;
  // The slot holding `ip`, claiming one when it has none (the first from its
  // home that holds no value); kNoSlot when every slot holds a value.
  size_t Claim(uint32_t ip) noexcept;
  // Empties `ip`'s slot, returning what was there.
  const Entry *Clear(uint32_t ip) noexcept;
  // Gives up `ip`'s claim of `slot`: its passes are taken back.
  void Release(uint32_t ip, size_t slot) noexcept;
  // Publishes `entry` in `slot`, returning the entry it replaced.
  const Entry *Publish(size_t slot, const Entry *entry) noexcept;
  // An entry this table's slab made, owned again (to retire it).
  Owned Own(const Entry *entry) noexcept {
    return Owned(entry, bess::utils::SlabDelete<const Entry>(slab_.get()));
  }

  // Released after the destructor body has destroyed the live entries; the
  // retired ones may outlive the table, and the slab goes with the last.
  bess::utils::ObjectSlab<Entry>::Owner slab_;
  std::vector<Slot> slots_;
  size_t mask_ = 0;
  size_t live_ = 0;  // slots holding a value (control side)
};

#endif  // BESS_MODULES_ARP_RESPONDER_H_
