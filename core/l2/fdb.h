// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_L2_FDB_H_
#define BESS_L2_FDB_H_

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <vector>

#include "dataplane/expiry_wheel.h"
#include "dataplane/interface_id.h"
#include "dataplane/strong_id.h"
#include "dataplane/table_policy.h"
#include "l2/mac_table.h"
#include "utils/common.h"

namespace bess::l2 {

// L2 forwarding database (roadmap M14, Decision D-064; experimental).
//
// Mechanism, not policy: exact (bridge domain, MAC) -> interface lookup,
// static programming, a learning helper, aging on the generic expiry wheel
// (M10), flood groups per domain, and a VLAN -> domain map. No STP, no EVPN, no
// learning-security policy, no controller intent: an application may disable
// learning entirely and program the FDB from a controller. No Module, gate or
// runtime dependency; interfaces are `dataplane::InterfaceId` (M7) and the
// owner maps them to gates, ports or hardware.
//
// Storage: the table is a type parameter (user decision 1, 2026-10-04: one
// FDB, the module picks the table). `Fdb` is the worker-owned MacTable: one
// worker learns, looks up, ages and programs. A storage says who may write and
// read it (dataplane/table_policy.h) and how many domains and how wide an
// interface it holds; every mutation holds its writer guard (nothing, for an
// owned table).
//
// Fail closed: a miss, an unknown domain and the invalid interface all read as
// kInvalidInterfaceId; the invalid interface cannot be programmed or learned.

struct BridgeDomainIdTag;
using BridgeDomainId = dataplane::StrongId<BridgeDomainIdTag, uint16_t>;

struct MacAddress {
  std::array<uint8_t, 6> bytes{};
  bool multicast() const noexcept { return (bytes[0] & 1) != 0; }
  friend bool operator==(const MacAddress &, const MacAddress &) = default;
};

// 8 bytes, no padding: hashed and compared as bytes.
struct FdbKey {
  uint16_t domain;
  uint8_t mac[6];
};
static_assert(sizeof(FdbKey) == 8);

// Built as one 64-bit word: two narrow stores read back by the hash's 8-byte
// load would defeat store-to-load forwarding (measured: 27 vs 10 ns a lookup).
inline FdbKey MakeKey(BridgeDomainId domain, const MacAddress &mac) noexcept {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "FdbKey word layout");
  uint32_t lo;
  uint16_t hi;
  std::memcpy(&lo, mac.bytes.data(), 4);
  std::memcpy(&hi, mac.bytes.data() + 4, 2);
  const uint64_t word = domain.value() | uint64_t{lo} << 16 | uint64_t{hi} << 48;
  FdbKey k;
  std::memcpy(&k, &word, sizeof(k));
  return k;
}

enum class FdbFlags : uint8_t {
  kDynamic = 0,  // learned; ages out
  kStatic = 1,   // programmed; never ages, never moved by learning
};

enum class LearnResult : uint8_t {
  kLearned,    // a new dynamic entry
  kRefreshed,  // same interface; age reset
  kMoved,      // the MAC moved to another interface; age reset
  kStatic,     // a static entry exists; nothing changed
  kIgnored,    // multicast source, invalid interface or unknown domain
  kFull,       // no room; nothing changed
};

enum class ProgramResult : uint8_t {
  kAdded,
  kReplaced,  // an entry (static or dynamic) for the key now has this value
  kInvalid,   // invalid interface, multicast MAC or unknown domain
  kFull,
};

// What BasicFdb needs from its table: 64-bit key words (domain | MAC, see
// MakeKey), a value per slot (the interface) and a flags byte, a cold word per
// slot for the aging timer, batch lookup, and the writer guard.
template <typename T>
concept FdbStorage = requires(T t, const T ct, uint64_t key, uint32_t slot,
                              std::span<const uint64_t> keys) {
  { T::Create(size_t{}) } -> std::same_as<std::unique_ptr<T>>;
  { ct.Lookup(key) } -> std::same_as<uint32_t>;
  { ct.LookupBatch(keys, static_cast<uint16_t *>(nullptr)) } -> std::same_as<uint64_t>;
  { ct.Find(key) } -> std::same_as<uint32_t>;
  { t.Insert(key, uint16_t{}, uint8_t{}) } -> std::same_as<uint32_t>;
  t.Erase(slot);
  { ct.value(slot) } -> std::convertible_to<uint32_t>;
  t.set_value(slot, uint16_t{});
  { ct.flags(slot) } -> std::same_as<uint8_t>;
  t.set_flags(slot, uint8_t{});
  t.ForEach([](uint32_t) {});
  { t.cold(slot) } -> std::same_as<dataplane::ExpiryHandle &>;
  { ct.size() } -> std::same_as<size_t>;
  { ct.capacity() } -> std::same_as<size_t>;
  { ct.memory_bytes() } -> std::same_as<size_t>;
  t.Lock();
  T::kNotFound;
  T::kMaxBatch;
  T::kMaxDomains;
  T::kMaxValue;
  typename T::writers;
  typename T::readers;
  typename T::growth;
};

template <typename Storage = MacTable<dataplane::ExpiryHandle>>
  requires FdbStorage<Storage>
class BasicFdb {
 public:
  using Tick = uint64_t;
  // The table; each slot's cold word is its aging timer.
  using Table = Storage;
  // The payload is the key word: every path that ends an entry's dynamic life
  // (removal, static reprogramming, flush) cancels its timer, so a timer that
  // fires belongs to the present dynamic entry for that key.
  using Wheel = dataplane::ExpiryWheel<uint64_t, Tick>;
  static constexpr size_t kMaxBatch = Table::kMaxBatch;

  struct Config {
    size_t capacity = 4096;      // entries, static and dynamic
    // Learning creates entries only while size() < learn_limit (0: capacity),
    // keeping room for static programming.
    size_t learn_limit = 0;
    Tick aging = 300'000'000'000;  // a learned entry is usable while now - learned <= aging
    unsigned granularity_shift = 20;  // wheel granularity: 2^shift ticks
    size_t max_domains = 4096;   // domain ids 0..max_domains-1
    Tick start = 0;
  };

  enum class CreateError : uint8_t { kInvalidConfig, kOutOfMemory };

  // Allocations happen only here (and in SetFloodGroup).
  static std::expected<std::unique_ptr<BasicFdb>, CreateError> Create(const Config &config) {
    if (config.capacity == 0 || config.capacity > (size_t{1} << 28) ||
        // Domain 0xFFFF is reserved: with the broadcast MAC its key word is the
        // table's empty marker, and it is kNoDomain.
        config.max_domains == 0 || config.max_domains > 65535 ||
        config.max_domains > Table::kMaxDomains) {
      return std::unexpected(CreateError::kInvalidConfig);
    }
    auto wheel = Wheel::Create(config.capacity, config.start, config.granularity_shift);
    if (!wheel) {
      return std::unexpected(wheel.error() == dataplane::ExpiryError::kOutOfMemory
                                 ? CreateError::kOutOfMemory
                                 : CreateError::kInvalidConfig);
    }
    auto table = Table::Create(config.capacity);
    if (table == nullptr) {
      return std::unexpected(CreateError::kOutOfMemory);
    }
    // The FDB allocates its flood groups as it is built: a refusal there is
    // kOutOfMemory like the others, not an exception.
    std::unique_ptr<BasicFdb> fdb;
    try {
      fdb.reset(new BasicFdb(std::move(table), std::move(*wheel), config));
    } catch (const std::bad_alloc &) {
      return std::unexpected(CreateError::kOutOfMemory);
    }
    return fdb;
  }

  // -- lookup -------------------------------------------------------------------

  // The interface for (domain, mac), or kInvalidInterfaceId (unknown: flood).
  dataplane::InterfaceId Lookup(BridgeDomainId domain, const MacAddress &mac) const noexcept {
    // Domain 0xFFFF with the broadcast MAC is the table's empty marker and
    // finds a free slot, whose value is 0: kInvalidInterfaceId, a miss. (A
    // domain bound check here measured +0.5 ns a lookup.)
    return dataplane::InterfaceId(
        static_cast<uint16_t>(table_->Lookup(Word(MakeKey(domain, mac)))));
  }

  // out[i] = interface or kInvalidInterfaceId; bit i set for a hit.
  uint64_t LookupBatch(std::span<const FdbKey> keys,
                       std::span<dataplane::InterfaceId> out) const noexcept {
    promise(keys.size() <= kMaxBatch && out.size() >= keys.size());
    uint64_t words[kMaxBatch];
    uint16_t values[kMaxBatch];
    for (size_t i = 0; i < keys.size(); i++) {
      words[i] = Word(keys[i]);
    }
    // A miss reads value 0, kInvalidInterfaceId.
    const uint64_t hits =
        table_->LookupBatch(std::span<const uint64_t>(words, keys.size()), values);
    for (size_t i = 0; i < keys.size(); i++) {
      out[i] = dataplane::InterfaceId(values[i]);
    }
    return hits;
  }

  // -- learning -----------------------------------------------------------------

  // Learns that `mac` in `domain` is reachable through `interface` at `now`.
  LearnResult Learn(BridgeDomainId domain, const MacAddress &mac,
                    dataplane::InterfaceId interface, Tick now) noexcept {
    if (mac.multicast() || interface == dataplane::kInvalidInterfaceId ||
        interface.value() > Table::kMaxValue || domain.value() >= max_domains_) {
      return LearnResult::kIgnored;
    }
    [[maybe_unused]] auto guard = table_->Lock();
    const uint64_t key = Word(MakeKey(domain, mac));
    // Usable while now - last learned <= aging (the legacy Bridge's rule), so
    // the timer fires one tick after that.
    const Tick deadline = Wheel::After(now, aging_ + 1);
    if (const uint32_t s = table_->Find(key); s != Table::kNotFound) {
      if (table_->flags(s) == static_cast<uint8_t>(FdbFlags::kStatic)) {
        return LearnResult::kStatic;
      }
      (void)wheel_->Refresh(table_->cold(s), deadline);
      if (table_->value(s) == interface.value()) {
        return LearnResult::kRefreshed;
      }
      table_->set_value(s, interface.value());
      return LearnResult::kMoved;
    }
    if (table_->size() >= learn_limit_) {
      return LearnResult::kFull;
    }
    const uint32_t s = table_->Insert(key, interface.value(),
                                      static_cast<uint8_t>(FdbFlags::kDynamic));
    if (s == Table::kNotFound) {
      return LearnResult::kFull;
    }
    // The wheel holds `capacity` timers, one per dynamic entry, but it
    // quarantines a node whose generation is exhausted (2^31 armings), so
    // Schedule can still refuse: an entry without a timer would never age.
    const dataplane::ExpiryHandle timer = wheel_->Schedule(deadline, key);
    if (timer == dataplane::kNoExpiry) {
      table_->Erase(s);
      return LearnResult::kFull;
    }
    table_->cold(s) = timer;
    return LearnResult::kLearned;
  }

  // Ages out dynamic entries whose deadline is <= now, doing at most `budget`
  // units of wheel work (see ExpiryWheel). Returns how many entries left.
  size_t Age(Tick now, size_t budget) noexcept {
    [[maybe_unused]] auto guard = table_->Lock();
    size_t removed = 0;
    (void)wheel_->Poll(now, budget, [this, &removed](const uint64_t &key) noexcept {
      const uint32_t s = table_->Find(key);
      if (s != Table::kNotFound && table_->flags(s) == static_cast<uint8_t>(FdbFlags::kDynamic)) {
        table_->Erase(s);
        removed++;
      }
    });
    return removed;
  }

  // -- programming (control side of the owning worker) ----------------------------

  // A static entry: never aged, never moved by learning.
  ProgramResult AddStatic(BridgeDomainId domain, const MacAddress &mac,
                          dataplane::InterfaceId interface) noexcept {
    if (interface == dataplane::kInvalidInterfaceId || mac.multicast() ||
        interface.value() > Table::kMaxValue || domain.value() >= max_domains_) {
      return ProgramResult::kInvalid;
    }
    [[maybe_unused]] auto guard = table_->Lock();
    const uint64_t key = Word(MakeKey(domain, mac));
    const uint32_t found = table_->Find(key);
    if (found == Table::kNotFound) {
      return table_->Insert(key, interface.value(), static_cast<uint8_t>(FdbFlags::kStatic)) ==
                     Table::kNotFound
                 ? ProgramResult::kFull
                 : ProgramResult::kAdded;
    }
    CancelTimer(found);
    table_->set_value(found, interface.value());
    table_->set_flags(found, static_cast<uint8_t>(FdbFlags::kStatic));
    return ProgramResult::kReplaced;
  }

  // Removes (domain, mac), static or dynamic. False if absent.
  bool Remove(BridgeDomainId domain, const MacAddress &mac) noexcept {
    if (domain.value() >= max_domains_) {
      return false;
    }
    [[maybe_unused]] auto guard = table_->Lock();
    const uint32_t s = table_->Find(Word(MakeKey(domain, mac)));
    if (s == Table::kNotFound) {
      return false;
    }
    CancelTimer(s);
    table_->Erase(s);
    return true;
  }

  // Removes every dynamic entry (static_too: every entry). O(capacity).
  void Flush(bool static_too = false) noexcept {
    [[maybe_unused]] auto guard = table_->Lock();
    table_->ForEach([&](uint32_t s) {
      if (static_too || table_->flags(s) == static_cast<uint8_t>(FdbFlags::kDynamic)) {
        CancelTimer(s);
        table_->Erase(s);
      }
    });
  }

  // -- flood groups -------------------------------------------------------------

  // The interfaces unknown-unicast and broadcast traffic of `domain` floods to.
  // Replaces the domain's group; at most kMaxFloodGroup interfaces; the invalid
  // interface is refused.
  static constexpr size_t kMaxFloodGroup = 64;
  bool SetFloodGroup(BridgeDomainId domain, std::span<const dataplane::InterfaceId> members) {
    if (domain.value() >= max_domains_ || members.size() > kMaxFloodGroup) {
      return false;
    }
    for (const auto m : members) {
      if (m == dataplane::kInvalidInterfaceId) {
        return false;
      }
    }
    flood_[domain.value()].assign(members.begin(), members.end());
    return true;
  }
  // The flood group, empty for an unknown domain.
  std::span<const dataplane::InterfaceId> FloodGroup(BridgeDomainId domain) const noexcept {
    if (domain.value() >= max_domains_) {
      return {};
    }
    return flood_[domain.value()];
  }

  // -- VLAN membership ---------------------------------------------------------

  // Maps an 802.1Q VLAN id (0..4095) to a domain; unmapped VLANs read as
  // kNoDomain (drop: fail closed).
  static constexpr BridgeDomainId kNoDomain{0xFFFF};
  void MapVlan(uint16_t vlan, BridgeDomainId domain) noexcept {
    if (vlan < vlan_domain_.size()) {
      vlan_domain_[vlan] = domain;
    }
  }
  BridgeDomainId DomainOfVlan(uint16_t vlan) const noexcept {
    return vlan < vlan_domain_.size() ? vlan_domain_[vlan] : kNoDomain;
  }

  // -- state ---------------------------------------------------------------------

  size_t size() const noexcept { return table_->size(); }
  size_t capacity() const noexcept { return table_->capacity(); }
  size_t dynamic_entries() const noexcept { return wheel_->size(); }
  size_t memory_bytes() const noexcept {
    return table_->memory_bytes() + wheel_->memory_bytes() + sizeof(*this);
  }

 private:
  BasicFdb(std::unique_ptr<Table> table, std::unique_ptr<Wheel> wheel, const Config &config)
      : table_(std::move(table)),
        wheel_(std::move(wheel)),
        aging_(config.aging),
        learn_limit_(config.learn_limit ? config.learn_limit : config.capacity),
        max_domains_(static_cast<uint32_t>(config.max_domains)),
        flood_(config.max_domains) {
    vlan_domain_.fill(kNoDomain);
  }

  static uint64_t Word(const FdbKey &k) noexcept {
    uint64_t w;
    std::memcpy(&w, &k, sizeof(w));
    return w;
  }
  void CancelTimer(uint32_t s) noexcept {
    if (table_->flags(s) == static_cast<uint8_t>(FdbFlags::kDynamic)) {
      (void)wheel_->Cancel(table_->cold(s));
      table_->cold(s) = dataplane::ExpiryHandle{};
    }
  }

  std::unique_ptr<Table> table_;
  std::unique_ptr<Wheel> wheel_;
  Tick aging_;
  size_t learn_limit_;
  uint32_t max_domains_;
  std::vector<std::vector<dataplane::InterfaceId>> flood_;
  std::array<BridgeDomainId, 4096> vlan_domain_;
};

// The worker-owned FDB (D-064): one worker learns, looks up, ages, programs.
using Fdb = BasicFdb<>;

}  // namespace bess::l2

#endif  // BESS_L2_FDB_H_
