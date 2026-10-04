// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_NAT_NAT_H_
#define BESS_NAT_NAT_H_

#include <rte_config.h>
#include <rte_hash_crc.h>
#include <rte_pause.h>
#include <rte_spinlock.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "conntrack/packet_parse.h"
#include "dataplane/expiry_wheel.h"
#include "flow/shared_flow_table.h"
#include "flow/worker_flow_table.h"
#include "rcu/rcu_domain.h"
#include "utils/checksum.h"
#include "utils/endian.h"
#include "utils/random.h"

namespace bess::nat {

// Endpoint-independent NAPT (RFC 4787) as a library (M18, D-068): the legacy
// NAT module's responsibilities -- endpoint keys, the binding table, address
// selection, port allocation, timeouts and the rewrite -- as separate pieces,
// with no gate or module. One worker owns a Nat.
//
//   Endpoint       <IPv4 address, port or ICMP identifier, protocol>, 8 bytes
//   AddressPool    external addresses with their port ranges; an internal
//                  address always maps to the same external one (REQ-2)
//   PortPool       a bitmap per (external address, protocol): allocation is a
//                  random start and a scan for a free bit, release clears it
//   Binding        one object per mapping, reachable from the internal
//                  endpoint (primary key) and the external one (alias)
//   Rewrite*       the typed translator: addresses, ports, identifiers and
//                  incremental checksums
//   Nat            the engine: Translate(frame, parsed, direction, now)

using utils::be16_t;
using utils::be32_t;

struct Endpoint {
  be32_t addr;  // bytes 0-3
  be16_t port;        // TCP/UDP port, or the ICMP query identifier
  uint16_t protocol;  // IP protocol, widened so the struct has no padding
  friend bool operator==(const Endpoint &a, const Endpoint &b) noexcept {
    return std::memcmp(&a, &b, sizeof(Endpoint)) == 0;
  }
};
static_assert(sizeof(Endpoint) == 8);
static_assert(std::has_unique_object_representations_v<Endpoint>);

// [begin, end) of ports or ICMP identifiers. A suspended range is not
// allocated from (the control plane may be draining it).
struct PortRange {
  uint16_t begin;
  uint32_t end;  // up to 65536
  bool suspended = false;
};

enum class Direction : uint8_t {
  kForward,  // internal -> external (outbound)
  kReverse,  // external -> internal (inbound)
};

enum class Verdict : uint8_t {
  kTranslated,
  kNotIpv4,         // parsed, but not IPv4
  kUnsupported,     // not TCP, UDP or an ICMP query (echo, timestamp, info)
  kNoBinding,       // reverse direction with no mapping
  kPortZero,        // TCP/UDP source port 0 (not mapped, as before)
  kExhausted,       // no free external port in any usable range
  kFull,            // the binding table or the wheel is full
  kConflict,        // the endpoint is another mapping's other side: outbound
                    // from an external endpoint, or a new mapping whose
                    // external endpoint is someone's internal one (one key
                    // space; refused, not mistranslated as before, D-068)
};

// -- rewrite (the direct typed translator) -----------------------------------------

namespace rewrite_internal {
inline uint16_t Load16(const uint8_t *p) noexcept {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
inline uint32_t Load32(const uint8_t *p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline void Store16(uint8_t *p, uint16_t v) noexcept { std::memcpy(p, &v, 2); }
inline void Store32(uint8_t *p, uint32_t v) noexcept { std::memcpy(p, &v, 4); }
}  // namespace rewrite_internal

// Rewrites the source (forward) or destination (reverse) endpoint of an IPv4
// packet from `before` to `after`, updating the IPv4 header checksum and the
// L4 one incrementally (RFC 1624): TCP always, UDP unless its checksum is 0
// (absent; a result of 0 is written as 0xffff, RFC 768), ICMP (no pseudo
// header: only the identifier; a result of 0 is written as 0xffff too, as an
// all-zero message -- echo reply, identifier 0, sequence 0, no payload --
// verifies only with 0xffff, and 0xffff verifies wherever 0 does). `ip` and
// `l4` are the packet's writable bytes at the offsets the parser checked;
// `before` is what the packet carries.
inline void Rewrite(uint8_t *ip, uint8_t *l4, Direction dir, const Endpoint &before,
                    const Endpoint &after) noexcept {
  using namespace rewrite_internal;
  using utils::ChecksumIncrement16;
  using utils::ChecksumIncrement32;
  using utils::UpdateChecksumWithIncrement;
  Store32(ip + (dir == Direction::kForward ? 12 : 16), after.addr.raw_value());
  const uint32_t l3 = ChecksumIncrement32(before.addr.raw_value(), after.addr.raw_value());
  Store16(ip + 10, UpdateChecksumWithIncrement(Load16(ip + 10), l3));
  const uint32_t l4inc = l3 + ChecksumIncrement16(before.port.raw_value(), after.port.raw_value());
  const uint8_t proto = ip[9];
  if (proto == 6 || proto == 17) {
    Store16(l4 + (dir == Direction::kForward ? 0 : 2), after.port.raw_value());
    if (proto == 6) {
      Store16(l4 + 16, UpdateChecksumWithIncrement(Load16(l4 + 16), l4inc));
    } else if (const uint16_t sum = Load16(l4 + 6); sum != 0) {
      const uint16_t updated = UpdateChecksumWithIncrement(sum, l4inc);
      Store16(l4 + 6, updated != 0 ? updated : 0xffff);
    }
  } else {
    Store16(l4 + 4, after.port.raw_value());
    const uint16_t updated = utils::UpdateChecksum16(Load16(l4 + 2), before.port.raw_value(),
                                                     after.port.raw_value());
    Store16(l4 + 2, updated != 0 ? updated : 0xffff);
  }
}

// -- address pool --------------------------------------------------------------------

struct ExternalAddress {
  be32_t addr;
  std::vector<PortRange> ranges;
};

// Which external address an internal address uses: always the same one
// (paired pooling, RFC 4787 REQ-2), by the legacy module's hash.
inline size_t PickAddress(be32_t internal, size_t count) noexcept {
  return rte_hash_crc(&internal, sizeof(be32_t), 0) % count;
}

// -- port pool -----------------------------------------------------------------------

// One bit per port for each (external address, protocol class). Allocation
// starts at a random port in the usable span and scans for a clear bit a
// 64-bit word at a time; a binding's release clears its bit. 8 KiB per
// address and class (TCP, UDP, ICMP).
class PortPool {
 public:
  explicit PortPool(size_t addresses) : bits_(addresses * kClasses) {}

  static int ClassOf(uint16_t protocol) noexcept {
    return protocol == 6 ? 0 : protocol == 17 ? 1 : 2;
  }

  // A free port in [lo, hi) for `address`, marked used, or nullopt. `start`
  // is the random offset into the span.
  std::optional<uint16_t> Allocate(size_t address, uint16_t protocol, uint32_t lo, uint32_t hi,
                                   uint32_t start) noexcept {
    if (lo >= hi) {
      return std::nullopt;
    }
    auto &bm = bits_[address * kClasses + ClassOf(protocol)];
    const uint32_t span = hi - lo;
    uint32_t p = lo + start % span;
    // Scan [p, hi) then [lo, p), a word at a time.
    for (int pass = 0; pass < 2; pass++) {
      const uint32_t end = pass == 0 ? hi : start % span + lo;
      while (p < end) {
        const uint32_t word = p / 64, bit = p % 64;
        uint64_t free = ~bm[word] & (~uint64_t{0} << bit);
        const uint32_t word_end = std::min<uint32_t>(end, (word + 1) * 64);
        if (word_end - word * 64 < 64) {
          free &= (uint64_t{1} << (word_end - word * 64)) - 1;
        }
        if (free != 0) {
          const uint32_t port = word * 64 + static_cast<uint32_t>(std::countr_zero(free));
          bm[word] |= uint64_t{1} << (port % 64);
          return static_cast<uint16_t>(port);
        }
        p = word_end;
      }
      p = lo;
    }
    return std::nullopt;
  }

  void Release(size_t address, uint16_t protocol, uint16_t port) noexcept {
    bits_[address * kClasses + ClassOf(protocol)][port / 64] &= ~(uint64_t{1} << (port % 64));
  }
  bool Used(size_t address, uint16_t protocol, uint16_t port) const noexcept {
    return bits_[address * kClasses + ClassOf(protocol)][port / 64] >> (port % 64) & 1;
  }
  size_t memory_bytes() const noexcept { return bits_.size() * sizeof(Bitmap); }

 private:
  static constexpr int kClasses = 3;
  using Bitmap = std::array<uint64_t, 65536 / 64>;
  std::vector<Bitmap> bits_;
};

// The span of external ports a mapping may use in `range` (the legacy
// module's classes, with its two off-by-one errors fixed, D-068): ICMP
// identifiers anywhere in the range; a privileged port (1-1023) to a
// privileged port (RFC 4787 REQ-5-a), never 0; any other port to 1024 and up.
// [lo, hi); empty when the range has none of that class.
inline std::pair<uint32_t, uint32_t> PortSpan(const PortRange &range, uint16_t protocol,
                                              uint16_t internal_port) noexcept {
  if (protocol != 6 && protocol != 17) {
    return {range.begin, range.end};
  }
  if (internal_port < 1024) {
    return {std::max<uint32_t>(1, range.begin), std::min<uint32_t>(1024, range.end)};
  }
  return {std::max<uint32_t>(1024, range.begin), range.end};
}

// -- the engine ----------------------------------------------------------------------

struct Binding {
  Endpoint internal;
  Endpoint external;
  uint32_t address_index;
  // The last outbound packet. An outbound packet refreshes the mapping with
  // this store only; the timer is re-armed from it when it fires (the
  // wheel's owner-kept deadline), so the hot path never touches the wheel.
  uint64_t last_refresh;
  dataplane::ExpiryHandle timer{};
};

struct BindingTraits : flow::DefaultFlowTableTraits {
  static constexpr size_t kAliases = 1;  // the external endpoint
};

// A binding shared by every worker (SharedNat): the endpoints are immutable
// once published; `last_refresh` is the one field workers write, a relaxed
// store (the latest outbound packet wins; expiry only needs "recently").
struct SharedBinding {
  Endpoint internal;
  Endpoint external;
  uint32_t address_index;
  std::atomic<uint64_t> last_refresh;
  dataplane::ExpiryHandle timer{};
  SharedBinding(Endpoint in, Endpoint ext, uint32_t a, uint64_t now)
      : internal(in), external(ext), address_index(a), last_refresh(now) {}
};

struct SharedBindingTraits : flow::DefaultSharedFlowTableTraits {
  static constexpr size_t kAliases = 1;  // the external endpoint
};

// How a NAT stores its bindings: the table policy (table_policy.md, user
// decisions 1 and 3; the module chooses).
//
// OwnedBindings: one worker owns the NAT; a WorkerFlowTable, no lock; fixed
// capacity (user decision 9.3), the code the NAT had before growth (D-068).
// GrowableOwnedBindings: the same, and the table may grow (TP5, D-078); the
// growth code is compiled only here, so a fixed NAT pays nothing for it.
template <bool Growable>
struct OwnedStore {
  static constexpr bool kShared = false;
  static constexpr bool kGrowable = Growable;
  using BindingType = Binding;
  using Table = flow::WorkerFlowTable<Endpoint, Binding, flow::DefaultFlowHash<Endpoint>,
                                      flow::DefaultFlowEqual<Endpoint>, BindingTraits>;
  static auto CreateTable(size_t capacity, rcu::RcuDomain *) { return Table::Create(capacity); }
  static uint64_t RefreshOf(const Binding &b) noexcept { return b.last_refresh; }
  static void Refresh(Binding &b, uint64_t now) noexcept { b.last_refresh = now; }
  struct Lock {  // nothing to exclude
    void Acquire() noexcept {}
    bool TryAcquire() noexcept { return true; }
    void Release() noexcept {}
  };
};
using OwnedBindings = OwnedStore<false>;
using GrowableOwnedBindings = OwnedStore<true>;

// SharedBindings: every worker translates through one NAT (any traffic
// distribution: the two directions of a mapping hash apart after the rewrite,
// so no steering keeps them on one worker; user decision 6). Lookups are the
// SharedFlowTable's, lock-free; creating a binding (port pick, both keys, the
// timer) holds the NAT's lock, which a worker waits for (decision 9.1); expiry
// runs on whichever worker takes the lock without waiting. Erased bindings are
// destroyed after an RCU grace period. Fixed capacity (TP6).
struct SharedBindings {
  static constexpr bool kShared = true;
  static constexpr bool kGrowable = true;
  using BindingType = SharedBinding;
  using Table = flow::SharedFlowTable<Endpoint, SharedBinding, SharedBindingTraits>;
  static auto CreateTable(size_t capacity, rcu::RcuDomain *domain) {
    return Table::Create(capacity, *domain);
  }
  static uint64_t RefreshOf(const SharedBinding &b) noexcept {
    return b.last_refresh.load(std::memory_order_relaxed);
  }
  static void Refresh(SharedBinding &b, uint64_t now) noexcept {
    b.last_refresh.store(now, std::memory_order_relaxed);
  }
  struct Lock {
    Lock() noexcept { rte_spinlock_init(&lock_); }
    void Acquire() noexcept { rte_spinlock_lock(&lock_); }
    bool TryAcquire() noexcept { return rte_spinlock_trylock(&lock_) != 0; }
    void Release() noexcept { rte_spinlock_unlock(&lock_); }
    rte_spinlock_t lock_;
  };
};

// 5 minutes (RFC 4787 REQ-5-c), refreshed by outbound packets only (REQ-6).
inline constexpr uint64_t kDefaultNatTimeout = 300ull * 1000 * 1000 * 1000;

struct NatConfig {
  std::vector<ExternalAddress> addresses;
  size_t capacity = 65536;  // bindings (user decision 3-B)
  // Larger than `capacity`: the table may grow to it, doubling, without
  // stopping (TP5: the owner asks, the control side allocates, the owner
  // migrates a few bindings a batch). 0 or <= capacity: fixed.
  size_t max_capacity = 0;
  uint64_t timeout = kDefaultNatTimeout;
  unsigned granularity_shift = 24;  // wheel granularity 2^24 ns, about 17 ms
  uint64_t start = 0;
  uint64_t seed = 0;  // port start randomness
  // SharedNat only: the domain whose grace periods free erased bindings
  // (every worker that translates is one of its readers).
  rcu::RcuDomain *rcu = nullptr;
};

enum class NatCreateError : uint8_t {
  kNoAddresses,
  kBadRange,
  kOutOfMemory,
  kInvalidCapacity,
  kNoRcuDomain,  // SharedNat without Config::rcu
};

template <typename Store>
class BasicNat {
 public:
  using Tick = uint64_t;  // nanoseconds, the caller's clock
  using Table = typename Store::Table;
  using BindingT = typename Store::BindingType;
  static constexpr bool kShared = Store::kShared;
  static constexpr bool kGrowable = Store::kGrowable;
  // A growable NAT's timers are keyed by the internal endpoint, not a slot
  // handle: a binding moves to a new table without touching its timer (TP5).
  using WheelKey = std::conditional_t<kGrowable, Endpoint, flow::FlowHandle>;
  using Wheel = dataplane::ExpiryWheel<WheelKey, Tick>;

  static constexpr Tick kDefaultTimeout = kDefaultNatTimeout;

  // The most mappings the addresses can serve: one per port and protocol
  // class (TCP, UDP, ICMP) of each distinct address, capped.
  static size_t CapacityFor(const std::vector<ExternalAddress> &addresses,
                            size_t cap = size_t{1} << 20) noexcept {
    size_t distinct = 0;
    for (size_t i = 0; i < addresses.size(); i++) {
      bool seen = false;
      for (size_t j = 0; j < i; j++) seen |= addresses[j].addr == addresses[i].addr;
      distinct += !seen;
    }
    return std::min(cap, distinct * 3 * 65536);
  }

  using Config = NatConfig;

  using CreateError = NatCreateError;

  static std::expected<std::unique_ptr<BasicNat>, CreateError> Create(const Config &config) {
    if (config.addresses.empty()) {
      return std::unexpected(CreateError::kNoAddresses);
    }
    for (const auto &a : config.addresses) {
      for (const auto &r : a.ranges) {
        if (r.begin >= r.end || r.end > 65536) {
          return std::unexpected(CreateError::kBadRange);
        }
      }
    }
    if (kShared && config.rcu == nullptr) {
      return std::unexpected(CreateError::kNoRcuDomain);
    }
    auto table = Store::CreateTable(config.capacity, config.rcu);
    if (!table) {
      return std::unexpected(table.error() == flow::FlowTableError::kOutOfMemory
                                 ? CreateError::kOutOfMemory
                                 : CreateError::kInvalidCapacity);
    }
    // Timers for the largest table the NAT may grow to: the wheel never moves.
    if (!kGrowable && config.max_capacity > config.capacity) {
      return std::unexpected(CreateError::kInvalidCapacity);  // use GrowableNat
    }
    auto wheel = Wheel::Create(std::max(config.capacity, config.max_capacity), config.start,
                               config.granularity_shift);
    if (!wheel) {
      return std::unexpected(CreateError::kOutOfMemory);
    }
    // The engine copies the addresses and allocates its port bitmaps as it
    // is built: a refusal there is kOutOfMemory like the others.
    std::unique_ptr<BasicNat> nat;
    try {
      nat.reset(new BasicNat(config, std::move(*table), std::move(*wheel)));
    } catch (const std::bad_alloc &) {
      return std::unexpected(CreateError::kOutOfMemory);
    }
    return nat;
  }

  // The endpoint a packet carries for `dir`: the source going out, the
  // destination coming in. nullopt for what is not translated (not IPv4, not
  // TCP/UDP/ICMP query).
  static std::optional<Endpoint> EndpointOf(const conntrack::ParsedFlowPacket &p,
                                            Direction dir, Verdict *why) noexcept {
    if (p.l3 != conntrack::L3Kind::kIpv4) {
      *why = Verdict::kNotIpv4;
      return std::nullopt;
    }
    const bool tcp_udp = p.l4 == conntrack::L4Kind::kTcp || p.l4 == conntrack::L4Kind::kUdp;
    const bool icmp_query = p.l4 == conntrack::L4Kind::kIcmp &&
                            (p.icmp_type == 0 || p.icmp_type == 8 || p.icmp_type == 13 ||
                             p.icmp_type == 14 || p.icmp_type == 15 || p.icmp_type == 16);
    if (!tcp_udp && !icmp_query) {
      *why = Verdict::kUnsupported;
      return std::nullopt;
    }
    // One 8-byte store: the hash reads the key as a word, and a word made of
    // narrow stores defeats store forwarding (measured, D-068).
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Endpoint word layout");
    uint32_t raw;  // network order, as on the wire
    std::memcpy(&raw, (dir == Direction::kForward ? p.src : p.dst).data(), 4);
    const uint16_t port = dir == Direction::kForward ? p.src_port : p.dst_port;
    const uint64_t word = uint64_t{raw} | uint64_t{be16_t::swap(port)} << 32 |
                          uint64_t{p.protocol} << 48;
    Endpoint e;
    std::memcpy(&e, &word, sizeof(e));
    return e;
  }

  // Translates one packet in place. `frame` is the packet's writable bytes and
  // `p` its parse (conntrack::ParseFrame, status kOk). Outbound packets with
  // no mapping get one; inbound ones without are kNoBinding.
  Verdict Translate(std::span<uint8_t> frame, const conntrack::ParsedFlowPacket &p, Direction dir,
                    Tick now) noexcept {
    Verdict why = Verdict::kTranslated;
    const std::optional<Endpoint> before = EndpointOf(p, dir, &why);
    if (!before) {
      return why;
    }
    return Finish(frame, p, dir, now, *before, FindAny(*before));
  }

  static constexpr size_t kMaxBatch = Table::kMaxBatch;

  // Translate for a batch: the lookups go together (FindBatch prefetches),
  // then each packet is finished in order. A packet `parsed_ok[i]` false is
  // skipped (verdict kUnsupported).
  void TranslateBatch(std::span<const std::span<uint8_t>> frames,
                      std::span<const conntrack::ParsedFlowPacket> parsed,
                      std::span<const bool> parsed_ok, Direction dir, Tick now,
                      std::span<Verdict> out) noexcept {
    promise(frames.size() <= kMaxBatch && parsed.size() == frames.size() &&
            parsed_ok.size() == frames.size() && out.size() >= frames.size());
    Endpoint keys[kMaxBatch];
    uint32_t where[kMaxBatch];
    size_t n = 0;
    for (size_t i = 0; i < frames.size(); i++) {
      Verdict why = Verdict::kUnsupported;
      std::optional<Endpoint> e;
      if (parsed_ok[i]) {
        e = EndpointOf(parsed[i], dir, &why);
      }
      out[i] = why;
      if (e) {
        keys[n] = *e;
        where[n++] = static_cast<uint32_t>(i);
      }
    }
    BindingT *found[kMaxBatch];
    (void)cur()->FindBatch(std::span<const Endpoint>(keys, n), std::span<BindingT *>(found, n));
    for (size_t k = 0; k < n; k++) {
      const size_t i = where[k];
      // A miss is looked up again: an earlier packet of this batch may have
      // just created the mapping.
      BindingT *b = found[k] != nullptr ? found[k] : FindAny(keys[k]);
      out[i] = Finish(frames[i], parsed[i], dir, now, keys[k], b);
    }
  }

 private:
  // The rest of Translate once the endpoint has been looked up (`b` the
  // binding its key found, or null).
  Verdict Finish(std::span<uint8_t> frame, const conntrack::ParsedFlowPacket &p, Direction dir,
                 Tick now, const Endpoint &before, BindingT *b) noexcept {
    // The internal endpoint is the primary key, the external one the alias:
    // a key that is the other side of a mapping belongs to another mapping.
    if (b != nullptr && !(dir == Direction::kForward ? b->internal == before
                                                     : b->external == before)) {
      if (dir == Direction::kForward) {
        return Verdict::kConflict;
      }
      b = nullptr;
    }
    if (b == nullptr) {
      if (dir == Direction::kReverse) {
        return Verdict::kNoBinding;
      }
      const Verdict made = Bind(before, now, &b);
      if (made != Verdict::kTranslated) {
        return made;
      }
    } else if (dir == Direction::kForward) {
      Store::Refresh(*b, now);
    }
    Rewrite(frame.data() + p.l3_offset, frame.data() + p.l4_offset, dir, before,
            dir == Direction::kForward ? b->external : b->internal);
    return Verdict::kTranslated;
  }

 public:

  // Removes mappings idle (no outbound packet) for the timeout, releasing
  // their ports. At most `budget` units of wheel work.
  size_t Expire(Tick now, size_t budget) noexcept {
    // Shared: one worker at a time, and never waiting (another worker that
    // holds the lock is creating or expiring; the next batch tries again).
    if (!lock_.TryAcquire()) {
      return 0;
    }
    if constexpr (kShared) {
      // Frees the slots of bindings erased by earlier calls whose grace period
      // has passed, and starts one for those erased since: an expired binding
      // returns to the free list a call or two later, not only when a create
      // finds the table full (which would refuse that create).
      (void)table_->Reclaim();
      if (old_ != nullptr) {
        (void)old_->Reclaim();
      }
    }
    size_t removed = 0;
    if constexpr (!kGrowable) {
      (void)wheel_->Poll(now, budget,
                         [this, now, &removed](const flow::FlowHandle &h) noexcept
                             -> std::optional<Tick> {
                           const BindingT *b = table_->Lookup(h);
                           if (b == nullptr) {
                             return std::nullopt;
                           }
                           // Refreshed since this deadline was set: wait for the new one.
                           const Tick due = Wheel::After(Store::RefreshOf(*b), timeout_);
                           if (static_cast<int64_t>(due - now) > 0) {
                             return due;
                           }
                           ports_.Release(PoolOf(b->address_index), b->external.protocol,
                                          b->external.port.value());
                           removed += table_->Erase(h) ? 1 : 0;
                           return std::nullopt;
                         });
    } else {
      (void)wheel_->Poll(now, budget,
                         [this, now, &removed](const Endpoint &internal) noexcept
                             -> std::optional<Tick> {
                           // Under the lock: the current table, and while growing
                           // the old one (owned: a binding not moved yet; shared:
                           // a binding not copied yet, or the copy's original).
                           BindingT *b = table_->Find(internal);
                           BindingT *o = old_ != nullptr ? old_->Find(internal) : nullptr;
                           if (b == nullptr) {
                             b = o;
                             o = nullptr;
                           }
                           if (b == nullptr) {
                             return std::nullopt;
                           }
                           // Refreshed since this deadline was set (in either copy):
                           // wait for the new one.
                           Tick last = Store::RefreshOf(*b);
                           if (o != nullptr && static_cast<int64_t>(Store::RefreshOf(*o) - last) > 0) {
                             last = Store::RefreshOf(*o);
                           }
                           const Tick due = Wheel::After(last, timeout_);
                           if (static_cast<int64_t>(due - now) > 0) {
                             return due;
                           }
                           ports_.Release(PoolOf(b->address_index), b->external.protocol,
                                          b->external.port.value());
                           bool erased = table_->Erase(internal);
                           if (old_ != nullptr) {
                             erased |= old_->Erase(internal);  // not moved, or the original
                           }
                           removed += erased ? 1 : 0;
                           return std::nullopt;
                         });
    }
    lock_.Release();
    return removed;
  }

  // -- growth (TP5) ----------------------------------------------------------------

  // Whether to ask for a larger table: at least 3/4 full, below the maximum,
  // and not already growing. One branch on a worker's path.
  bool NeedsGrowth() const noexcept {
    if constexpr (!kGrowable) {
      return false;
    }
    const Table *t = cur();
    return prev() == nullptr && t->capacity() < max_capacity_ &&
           t->size() * 4 >= t->capacity() * 3;
  }
  // The capacity to ask for: double, at most the maximum.
  size_t GrowthTarget() const noexcept {
    return std::min(cur()->capacity() * 2, max_capacity_);
  }

  // Shared growth (TP6, table_policy.md 4.2, the rhashtable shape): on the
  // control thread, which is not a reader of the NAT's RCU domain. Allocates
  // the larger table; publishes it (creates go to it, lookups try it, then the
  // old one); copies the old table into it in chunks under the lock, released
  // between chunks so workers keep creating; stops lookups in the old table
  // and waits for a grace period; folds refreshes that reached old copies
  // meanwhile; frees the old table. Workers never stop. False (and nothing
  // changes) when the table cannot be allocated.
  bool Grow(size_t capacity)
    requires(kShared)
  {
    if (capacity <= cur()->capacity() || capacity > max_capacity_) {
      return true;
    }
    auto made = Store::CreateTable(capacity, rcu_);
    if (!made) {
      return false;
    }
    lock_.Acquire();
    old_ = std::move(table_);
    table_ = std::move(*made);
    prev_.store(old_.get(), std::memory_order_release);
    cur_.store(table_.get(), std::memory_order_release);
    lock_.Release();
    std::vector<Endpoint> unplaced;  // the control thread may allocate
    for (size_t cursor = 0; cursor < old_->capacity();) {
      lock_.Acquire();
      cursor = old_->VisitRange(cursor, kGrowChunk,
                                [this, &unplaced](flow::FlowHandle, const Endpoint &internal,
                                                  const BindingT &b) {
                                  // Absent from the new table: creates check the
                                  // old one under this lock first. The new table
                                  // has room (capacity counts both), but its
                                  // directory may refuse a key (kPlacementFailed).
                                  auto copy = table_->EmplaceAliased(
                                      internal, b.external, b.internal, b.external,
                                      b.address_index, Store::RefreshOf(b));
                                  if (copy.created()) {
                                    copy.state->timer = b.timer;
                                  } else if (copy.status != flow::EmplaceStatus::kExists) {
                                    unplaced.push_back(internal);
                                  }
                                });
      // A binding the new table refused ends here, cleanly: its port goes back
      // to the pool and it leaves the old table (its timer will find nothing).
      // The flow gets a new mapping with its next outbound packet. Counted.
      for (const Endpoint &internal : unplaced) {
        if (const BindingT *b = old_->Find(internal)) {
          ports_.Release(PoolOf(b->address_index), b->external.protocol,
                         b->external.port.value());
          (void)old_->Erase(internal);
          dropped_in_growth_++;
        }
      }
      unplaced.clear();
      lock_.Release();
    }
    lock_.Acquire();
    prev_.store(nullptr, std::memory_order_release);
    lock_.Release();
    rcu_->Synchronize();  // no worker still holds an old binding
    lock_.Acquire();
    old_->ForEach([this](flow::FlowHandle, const Endpoint &internal, const BindingT &b) {
      if (BindingT *n = table_->Find(internal)) {
        const Tick old_refresh = Store::RefreshOf(b);
        if (static_cast<int64_t>(old_refresh - Store::RefreshOf(*n)) > 0) {
          Store::Refresh(*n, old_refresh);
        }
      }
    });
    std::unique_ptr<Table> retired = std::move(old_);
    migrated_tables_++;
    lock_.Release();
    retired.reset();  // its destructor waits for a grace period again
    return true;
  }
  // Slots copied per lock hold while a shared NAT grows (about 64 x 50 ns).
  static constexpr size_t kGrowChunk = 64;
  // A new, empty table: the control side calls this (allocation never happens
  // on the packet path) and hands the result to the owner.
  static std::unique_ptr<Table> NewTable(size_t capacity)
    requires(!kShared && kGrowable)
  {
    auto made = Table::Create(capacity);
    return made ? std::move(*made) : nullptr;
  }
  // The owner takes over a larger table: new bindings go to it, lookups try it
  // first, then the old one, until MigrateSome has moved every binding.
  void Adopt(std::unique_ptr<Table> bigger) noexcept
    requires(!kShared && kGrowable)
  {
    if (bigger == nullptr || old_ != nullptr || bigger->capacity() <= table_->capacity()) {
      return;  // stale or useless: dropped (freed by the caller's unique_ptr)
    }
    old_ = std::move(table_);
    table_ = std::move(bigger);
    cursor_ = 0;
  }
  bool migrating() const noexcept { return prev() != nullptr; }
  // Moves the bindings of the next `slots` slots of the old table to the new
  // one (an owned table has one thread: a binding is in exactly one table at
  // every lookup, and size() stays exact). When the walk ends, returns the old,
  // now empty, table for the caller to free off the packet path; nullptr until
  // then.
  std::unique_ptr<Table> MigrateSome(size_t slots) noexcept
    requires(!kShared && kGrowable)
  {
    if (old_ == nullptr) {
      return nullptr;
    }
    flow::FlowHandle moved[kMaxMigrateSlots];
    size_t n = 0;
    cursor_ = old_->VisitRange(cursor_, std::min(slots, kMaxMigrateSlots),
                               [&](flow::FlowHandle h, const Endpoint &internal, BindingT &b) {
                                 // The larger table has room, and creates check
                                 // both tables first, so this always creates.
                                 if (table_->EmplaceAliased(internal, *old_->AliasOf(h, 0), b)
                                         .created()) {
                                   moved[n++] = h;
                                 }
                               });
    for (size_t i = 0; i < n; i++) {
      (void)old_->Erase(moved[i]);
    }
    if (cursor_ < old_->capacity()) {
      return nullptr;
    }
    migrated_tables_++;
    return std::move(old_);
  }
  static constexpr size_t kMaxMigrateSlots = 256;
  size_t migrated_tables() const noexcept { return migrated_tables_; }
  // Shared growth: bindings the larger table's directory could not place, ended
  // (port released) instead of copied. Expected 0; nonzero is visible.
  size_t dropped_in_growth() const noexcept { return dropped_in_growth_; }
  size_t capacity() const noexcept { return cur()->capacity(); }

  const BindingT *Find(const Endpoint &e) const noexcept {
    return const_cast<BasicNat *>(this)->FindAny(e);
  }
  // Bindings. Exact for an owned NAT; while a shared NAT grows, a binding and
  // its copy count twice.
  size_t size() const noexcept {
    const Table *o = prev();
    return cur()->size() + (o != nullptr ? o->size() : 0);
  }
  const std::vector<ExternalAddress> &addresses() const noexcept { return addresses_; }
  // Bytes for `capacity` bindings: the table (both keys and the binding; for a
  // SharedNat its slots, excluding the rte_hash directory), the wheel and the
  // port bitmaps.
  size_t memory_bytes() const noexcept {
    const Table *o = prev();
    return cur()->memory_bytes() + (o != nullptr ? o->memory_bytes() : 0) +
           wheel_->memory_bytes() + ports_.memory_bytes();
  }

 private:
  BasicNat(const Config &config, std::unique_ptr<Table> table, std::unique_ptr<Wheel> wheel)
      : addresses_(config.addresses),
        timeout_(config.timeout),
        table_(std::move(table)),
        wheel_(std::move(wheel)),
        ports_(config.addresses.size()) {
    rng_.SetSeed(config.seed);
    max_capacity_ = std::max(config.capacity, config.max_capacity);
    rcu_ = config.rcu;
    if constexpr (kShared) {
      cur_.store(table_.get(), std::memory_order_relaxed);
    }
    // One bitmap per distinct external IP: two entries for one address (with
    // overlapping ranges) share it, so a port is never handed out twice.
    pool_of_.resize(addresses_.size());
    for (size_t i = 0; i < addresses_.size(); i++) {
      pool_of_[i] = static_cast<uint32_t>(i);
      for (size_t j = 0; j < i; j++) {
        if (addresses_[j].addr == addresses_[i].addr) {
          pool_of_[i] = pool_of_[j];
          break;
        }
      }
    }
  }

  size_t PoolOf(size_t address_index) const noexcept { return pool_of_[address_index]; }

  // The tables lookups use: the current one and, while growing, the old one.
  // Owned: the owner's pointers. Shared: published for lock-free readers.
  Table *cur() const noexcept {
    if constexpr (kShared) {
      return cur_.load(std::memory_order_acquire);
    } else {
      return table_.get();
    }
  }
  Table *prev() const noexcept {
    if constexpr (!kGrowable) {
      return nullptr;
    } else if constexpr (kShared) {
      return prev_.load(std::memory_order_acquire);
    } else {
      return old_.get();
    }
  }

  // The binding `e` names in the current table, else (while growing) in the
  // old one. Outside growth this is one lookup and one predicted branch.
  BindingT *FindAny(const Endpoint &e) noexcept {
    BindingT *b = cur()->Find(e);
    if (b == nullptr) [[unlikely]] {
      if (Table *o = prev(); o != nullptr) [[unlikely]] {
        b = FindIn(*o, e);
      }
    }
    return b;
  }
  // Out of line and cold: the growth-only lookup stays out of the hot loop.
  [[gnu::noinline, gnu::cold]] static BindingT *FindIn(Table &t, const Endpoint &e) noexcept {
    return t.Find(e);
  }

  Verdict Bind(const Endpoint &internal, Tick now, BindingT **out) noexcept {
    if ((internal.protocol == 6 || internal.protocol == 17) && internal.port == be16_t(0)) {
      return Verdict::kPortZero;
    }
    lock_.Acquire();
    const Verdict v = BindLocked(internal, now, out);
    lock_.Release();
    return v;
  }

  // Under lock_ (shared; nothing to hold when owned).
  Verdict BindLocked(const Endpoint &internal, Tick now, BindingT **out) noexcept {
    // Bindings in both tables count while migrating: the new table must keep
    // room for every binding not moved yet.
    if (wheel_->full()) {
      return Verdict::kFull;
    }
    if constexpr (!kShared) {
      if (table_->full()) {
        return Verdict::kFull;
      }
    }
    if constexpr (kGrowable) {
      // (Shared: conservative while growing, a copied binding counts twice.)
      if (table_->size() + (old_ != nullptr ? old_->size() : 0) >= table_->capacity()) {
        return Verdict::kFull;
      }
    }
    if (kGrowable && old_ != nullptr) [[unlikely]] {
      // Growing: the old table is part of the key space. Shared: another
      // worker may have bound this endpoint there after our lookup.
      if (BindingT *x = FindIn(*old_, internal)) {
        if (x->internal == internal) {
          *out = x;
          return Verdict::kTranslated;
        }
        return Verdict::kConflict;
      }
    }
    const size_t a = PickAddress(internal.addr, addresses_.size());
    for (const PortRange &range : addresses_[a].ranges) {
      if (range.suspended) {
        continue;
      }
      const auto [lo, hi] = PortSpan(range, internal.protocol, internal.port.value());
      const auto port = ports_.Allocate(PoolOf(a), internal.protocol, lo, hi, rng_.Get());
      if (!port) {
        continue;
      }
      const Endpoint external{addresses_[a].addr, be16_t(*port), internal.protocol};
      if (kGrowable && old_ != nullptr && FindIn(*old_, external) != nullptr) [[unlikely]] {
        // The external endpoint is an internal one's key in the old table.
        ports_.Release(PoolOf(a), internal.protocol, *port);
        return Verdict::kConflict;
      }
      auto made = table_->EmplaceAliased(
          internal, external, internal, external, static_cast<uint32_t>(a), now);
      if (!made.created()) {
        ports_.Release(PoolOf(a), internal.protocol, *port);
        if constexpr (kShared) {
          // Another worker bound this endpoint between our lookup and the lock:
          // use its binding (the endpoint is that flow's primary key).
          if (made.status == flow::EmplaceStatus::kExists && made.state != nullptr &&
              made.state->internal == internal) {
            *out = made.state;
            return Verdict::kTranslated;
          }
        }
        // The table is full, or the external endpoint is an internal one's
        // key: refuse.
        return made.status == flow::EmplaceStatus::kFull ? Verdict::kFull : Verdict::kConflict;
      }
      WheelKey key;
      if constexpr (kGrowable) {
        key = internal;
      } else {
        key = made.handle;
      }
      made.state->timer = wheel_->Schedule(Wheel::After(now, timeout_), key);
      if (made.state->timer == dataplane::kNoExpiry) {
        ports_.Release(PoolOf(a), internal.protocol, *port);
        (void)table_->Erase(made.handle);
        return Verdict::kFull;
      }
      *out = made.state;
      return Verdict::kTranslated;
    }
    return Verdict::kExhausted;
  }

  std::vector<ExternalAddress> addresses_;
  Tick timeout_;
  std::unique_ptr<Table> table_;
  std::unique_ptr<Wheel> wheel_;
  PortPool ports_;
  std::vector<uint32_t> pool_of_;  // address index -> its IP's bitmap
  Random rng_;
  // Growth state (TP5), last: the members above keep their offsets.
  size_t max_capacity_;
  std::unique_ptr<Table> old_;  // while migrating: the table being moved from
  size_t cursor_ = 0;           // its next slot to move
  size_t migrated_tables_ = 0;
  size_t dropped_in_growth_ = 0;
  typename Store::Lock lock_;
  rcu::RcuDomain *rcu_ = nullptr;
  // Shared: the tables published to lock-free readers (owned: unused).
  std::atomic<Table *> cur_{nullptr};
  std::atomic<Table *> prev_{nullptr};
};

// The owned NAT (one worker; fixed, or growable) and the shared one (every
// worker, growable).
using Nat = BasicNat<OwnedBindings>;
using GrowableNat = BasicNat<GrowableOwnedBindings>;
using SharedNat = BasicNat<SharedBindings>;

}  // namespace bess::nat

#endif  // BESS_NAT_NAT_H_
