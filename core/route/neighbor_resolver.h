// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_NEIGHBOR_RESOLVER_H_
#define BESS_ROUTE_NEIGHBOR_RESOLVER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/expiry_wheel.h"
#include "dataplane/interface_id.h"
#include "route/l3_packet.h"
#include "route/neighbor_table.h"
#include "route/next_hop_id.h"
#include "route/router.h"
#include "utils/ether.h"
#include "utils/ip.h"

namespace bess::route {

// IPv4 neighbor resolution over Ethernet (ARP) and IPv6 Neighbor Discovery:
// solicitation with retries and backoff, learning, aging, MAC changes,
// failure and recovery (M15, D-175).
//
// The resolver is the event source for the neighbor table
// (`neighbor_table.h`): it owns one neighbor entry per (interface, address),
// created by Bind, and moves it through
//
//   kIncomplete --reply--> kResolved --aging--> kResolved, reconfirming
//        |                    ^   (keeps its MAC; solicits)  |      |
//        |                    +-----------reply--------------+      |
//        +--max probes--> kUnreachable <------max probes------------+
//                              |
//                              +--retry--> kIncomplete
//
// Aging does not take a resolved neighbor out of service (RFC 4861's STALE
// and PROBE states, D-183): when its reachable lifetime ends it keeps
// forwarding on the MAC it has while the resolver solicits again, with the
// same probe count and backoff as a first attempt. A reply refreshes it; only
// an attempt that sends max_probes without one fails it to kUnreachable, which
// clears the MAC. So a live neighbor never drops traffic for being re-checked,
// and a dead one is out of service after at most the probe budget.
//
// Every transition calls NeighborTable::Update and returns the next hops to
// republish, so the caller still publishes adjacency with the existing
// mechanism -- Router::SetNextHop through Publish(), or one transaction built
// from NextHopOps(). The library sends and parses packets but writes no
// packet and no route: the caller owns the graph, the gates, the buffers and
// the policy for a frame whose next hop is unresolved.
//
// Identity. A neighbor is named by (dataplane::InterfaceId, address), the
// same key as NeighborTable. The interface also has an identity of its own --
// its MAC and its addresses, any number of each family with their prefixes --
// and every entry remembers the interface generation it was created under. A
// solicited reply is accepted only when all of these hold:
//   - the frame arrives on the interface the entry belongs to;
//   - the entry's interface generation is still current;
//   - the reply's Ethernet source and ARP sender match, and the frame is
//     addressed to the interface's MAC (or, for ARP, broadcast);
//   - the reply's sender address is the entry's address;
//   - the reply's target address is one of the interface's current addresses;
//   - the sender's MAC is a usable unicast address.
// A stale reply -- one in flight when the interface's address moved, one that
// arrives on another interface, or one for a neighbor that was unbound and
// rebound -- is ignored rather than applied to a newer entry. ARP that
// answers nothing (a request, a gratuitous announcement) still moves a bound
// neighbor to the MAC it announces, as RFC 826's merge does, and has it
// reconfirmed (OnArp). Nothing ever creates a neighbor: only Bind does, so an
// unsolicited frame cannot populate the table.
//
// Timers. The resolver reuses the worker-owned ExpiryWheel
// (`dataplane/expiry_wheel.h`) instead of keeping a second timer wheel. Each
// neighbor has at most one armed timer, its capacity is the configured
// neighbor capacity, and a payload carries the entry's arming serial so a
// record that outlives its entry cannot act on a newer one. A live timer is
// re-armed in place (same node), so replies spend no wheel capacity; a new
// node is needed only for an entry without one, and if the wheel refuses it
// (quarantined nodes lower the usable capacity) the caller never keeps an
// untimed neighbor: a reply is dropped, Bind rolls back, and a Poll or
// ConfigureInterface transition that cannot arm removes the entry. The
// resolver is
// single-threaded: its writer (a module's control side, the maintenance
// thread, or an application's own thread) calls Bind, ConfigureInterface,
// OnArp and Poll; packets read only the published NextHop objects, which are
// immutable and swapped by one pointer store, so a lookup sees the old
// adjacency or the new one, never a half-written entry. Poll must be called
// with a non-decreasing clock and at least once per the wheel's kMaxTimeout.
//
// Time is in caller ticks (see docs/expiry.md): the library reads no clock and
// knows no rate. Timing values are therefore ticks too, and zero has the
// documented meaning at each field.
//
// IPv4 ARP and IPv6 Neighbor Discovery use the same address-family-tagged
// entries; packet helpers remain family-specific.
class NeighborResolver {
 public:
  using Key = NeighborTable::Key;

  // Timing, in ticks. Defaults are plausible for a 1 ms tick; a caller with
  // another clock sets its own.
  struct Timing {
    // Delay from Bind to the first solicitation. Zero solicits on the next
    // Poll.
    uint64_t first_probe = 1000;
    // Retry backoff: probe_interval, 2*probe_interval, ... capped at
    // max_probe_interval.
    uint64_t probe_interval = 1000;
    uint64_t max_probe_interval = 8000;
    // Solicitations per resolution attempt (1..). The attempt fails after the
    // last one, when its retry timer fires.
    uint32_t max_probes = 3;
    // How long a resolved neighbor goes unconfirmed before it is solicited
    // again. It keeps forwarding on its MAC while that attempt runs and fails
    // only when the attempt does (max_probes without a reply). Zero disables
    // aging: a resolved neighbor stays resolved until a reply changes it or it
    // is unbound.
    uint64_t reachable_lifetime = 30000;
    // How long an unreachable neighbor waits before another attempt; the
    // default is the backoff ceiling, so a failed neighbor keeps being probed
    // at that rate. Zero means no further attempt: it stays unreachable until
    // a reply arrives or the caller removes and rebinds it.
    uint64_t unreachable_retry = 8000;
  };

  struct Config {
    // Bound on neighbors and therefore on armed timers. Every entry has at
    // most one timer, so Create() allocates exactly this many wheel nodes.
    size_t capacity = 1024;
    // ExpiryWheel's granularity shift (0: one tick).
    unsigned granularity_shift = 0;
    Timing timing;
  };

  // One address an interface holds, with its on-link prefix length: the
  // prefix picks which address a solicitation for a neighbor in it leaves
  // from. A host address is /32 or /128.
  struct InterfaceAddress {
    NeighborAddress address;
    uint8_t prefix_length = 0;

    // Whether `other` is in this address's prefix (same family only).
    bool Contains(const NeighborAddress &other) const noexcept {
      if (other.family != address.family) {
        return false;
      }
      const unsigned whole = prefix_length / 8u;
      if (std::memcmp(address.bytes, other.bytes, whole) != 0) {
        return false;
      }
      const unsigned rest = prefix_length % 8u;
      if (rest == 0) {
        return true;
      }
      const auto mask = static_cast<uint8_t>(0xffu << (8u - rest));
      return (address.bytes[whole] & mask) == (other.bytes[whole] & mask);
    }

    friend bool operator==(const InterfaceAddress &,
                           const InterfaceAddress &) = default;
  };

  // The identity of an interface for resolution: what it sends from (its MAC
  // and addresses) and how it is named. Addresses of both families, in
  // configuration order: a solicitation leaves from the address whose prefix
  // holds the neighbor, else from the first address of the neighbor's family;
  // an interface with no address of a family sends no solicitation for it.
  struct Interface {
    dataplane::InterfaceId id;
    utils::Ethernet::Address mac;
    std::vector<InterfaceAddress> addresses;
  };

  // One solicitation to put on the wire: an ARP request or a Neighbor
  // Solicitation. `frame` has room for either (kMaxNeighborFrameSize);
  // `length` is what the packet helper wrote, and a port that needs the
  // 60-byte Ethernet minimum pads it.
  struct Solicitation {
    dataplane::InterfaceId egress;
    std::array<uint8_t, kMaxNeighborFrameSize> frame;
    size_t length = 0;
  };

  // What one call produced: frames to send and neighbor changes to publish.
  // A change is present only when the neighbor's state or MAC actually
  // changed; the caller forwards `updates` to NeighborTable::Publish or to
  // NextHopOps() and a transaction.
  struct Progress {
    std::vector<Solicitation> solicitations;
    NeighborTable::Updates updates;

    bool empty() const noexcept {
      return solicitations.empty() && updates.empty();
    }
  };

  // `start` is the wheel's starting time; Poll()'s clock must be
  // non-decreasing from it. Returns an error for a zero capacity, a zero
  // max_probes or an invalid granularity.
  static std::expected<std::unique_ptr<NeighborResolver>, std::string> Create(
      const Config &config, uint64_t start = 0) {
    if (config.capacity == 0) {
      return std::unexpected("neighbor capacity must be non-zero");
    }
    if (config.timing.max_probes == 0) {
      return std::unexpected("max_probes must be non-zero");
    }
    auto wheel = dataplane::ExpiryWheel<TimerPayload>::Create(
        config.capacity, start, config.granularity_shift);
    if (!wheel) {
      return std::unexpected(dataplane::ToString(wheel.error()));
    }
    return std::unique_ptr<NeighborResolver>(
        new NeighborResolver(config, std::move(*wheel)));
  }

  // What Create allocates (D-198), all of it glibc: the timer wheel (one
  // node per neighbor), the due list reserved for every neighbor, and the
  // resolver itself, as bytes() counts them. The control-side maps of
  // neighbors and interfaces grow with them and are not counted.
  static size_t Bytes(const Config &config, uint64_t start = 0) noexcept {
    return dataplane::ExpiryWheel<TimerPayload>::Bytes(
               config.capacity, start, config.granularity_shift) +
           config.capacity * sizeof(TimerPayload) + sizeof(NeighborResolver);
  }
  static size_t GlibcBytes(const Config &config, uint64_t start = 0) noexcept {
    return Bytes(config, start);
  }
  size_t bytes() const noexcept {
    return wheel_->bytes() + due_.capacity() * sizeof(TimerPayload) +
           sizeof(*this);
  }

  NeighborResolver(const NeighborResolver &) = delete;
  NeighborResolver &operator=(const NeighborResolver &) = delete;

  // -- control side -----------------------------------------------------------

  // Adds an interface or replaces its identity. A changed MAC or address list
  // starts a new interface generation: every neighbor on it is marked
  // incomplete, its pending solicitation is cancelled, and resolution starts
  // again from the new identity. The same identity again returns an empty
  // Progress. `now` is the clock for the new first probes. An address that
  // cannot be an interface's (unspecified, broadcast, multicast, a prefix
  // longer than its family) or a repeated one is refused.
  std::expected<Progress, std::string> ConfigureInterface(const Interface &iface,
                                                          uint64_t now) {
    for (size_t i = 0; i < iface.addresses.size(); i++) {
      const InterfaceAddress &entry = iface.addresses[i];
      if (entry.address.is_ipv4()) {
        const uint32_t v4 = entry.address.ipv4();
        if (v4 == 0 || v4 == 0xffffffffu || entry.prefix_length > 32) {
          return std::unexpected("invalid interface address");
        }
      } else {
        const utils::Ipv6Address v6 = entry.address.ipv6();
        if (v6.IsUnspecified() || v6.IsMulticast() || entry.prefix_length > 128) {
          return std::unexpected("invalid interface address");
        }
      }
      for (size_t j = 0; j < i; j++) {
        if (iface.addresses[j].address == entry.address) {
          return std::unexpected("repeated interface address");
        }
      }
    }
    auto it = interfaces_.find(iface.id);
    if (it != interfaces_.end() && it->second.mac == iface.mac &&
        it->second.addresses == iface.addresses) {
      return Progress{};
    }
    uint32_t epoch = it == interfaces_.end() ? 1 : it->second.epoch + 1;
    if (epoch == 0) {
      epoch = 1;  // wrapped: no entry can still name the old generation
    }
    StoredInterface stored{iface.mac, iface.addresses, epoch};

    // Stage every neighbor key on the interface -- bound or not, so retained
    // learning is invalidated -- before committing the identity or touching
    // a table, timer or resolver entry. A refused allocation leaves the old
    // identity in place, so the same call retried converges.
    Progress out = NewProgress();
    const std::vector<Key> keys = neighbors_.KeysForInterface(iface.id);
    std::vector<NeighborTable::Prepared> prepared;
    prepared.reserve(keys.size());
    size_t update_count = 0;
    for (const Key &key : keys) {
      prepared.push_back(neighbors_.Prepare(key, NeighborState::kIncomplete,
                                           utils::Ethernet::Address{},
                                           iface.mac));
      update_count += prepared.back().updates.size();
    }
    out.updates.reserve(update_count);

    // Commit. insert_or_assign may allocate only for an interface id not seen
    // before (the address list was copied above), and only before any
    // neighbor is committed; if it throws, nothing else has changed and the
    // same call retried converges.
    interfaces_.insert_or_assign(iface.id, std::move(stored));
    for (NeighborTable::Prepared &update : prepared) {
      neighbors_.Commit(update);
      AppendUpdates(std::move(update.updates), out);
      auto entry = entries_.find(update.key);
      if (entry != entries_.end()) {
        entry->second.epoch = epoch;
        entry->second.state = NeighborState::kIncomplete;
        entry->second.probes = 0;
        entry->second.reconfirming = false;
        if (!Arm(update.key, entry->second, now, timing_.first_probe)) {
          // No usable timer node: forget the entry rather than keep a
          // neighbor that can never solicit. The committed update already
          // leaves the hop incomplete, which is what resolution being
          // unavailable looks like to the dataplane.
          Disarm(entry->second);
          entries_.erase(entry);
        }
      }
    }
    return out;
  }

  // Removes the interface: every neighbor on it becomes unreachable and its
  // binding stays in the neighbor table (the next hops remain readable), but
  // the resolver forgets its state and timers. A later ConfigureInterface
  // does not revive those neighbors; Bind them again. All updates are staged
  // before the interface or any neighbor changes; a refused allocation
  // leaves the interface and every entry as they were.
  Progress RemoveInterface(dataplane::InterfaceId id) {
    Progress out = NewProgress();
    const std::vector<Key> keys = neighbors_.KeysForInterface(id);
    std::vector<NeighborTable::Prepared> prepared;
    prepared.reserve(keys.size());
    size_t update_count = 0;
    for (const Key &key : keys) {
      prepared.push_back(neighbors_.Prepare(
          key, NeighborState::kUnreachable, utils::Ethernet::Address{}));
      update_count += prepared.back().updates.size();
    }
    out.updates.reserve(update_count);

    interfaces_.erase(id);
    for (NeighborTable::Prepared &update : prepared) {
      neighbors_.Commit(update);
      auto entry = entries_.find(update.key);
      if (entry != entries_.end()) {
        Disarm(entry->second);
        entries_.erase(entry);
      }
      AppendUpdates(std::move(update.updates), out);
    }
    return out;
  }

  // Whether `key` can be bound: its interface is configured and holds an
  // address of the neighbor's family to solicit from, and the address is a
  // usable unicast neighbor that is not one of the interface's own. Bind
  // refuses exactly what this refuses (and, beyond it, only a full table or
  // timer wheel), so an owner can check a binding before committing to it.
  std::expected<void, std::string> CheckBind(Key key) const {
    auto iface = interfaces_.find(key.interface);
    if (iface == interfaces_.end()) {
      return std::unexpected("unknown interface");
    }
    if (key.address.is_ipv4()) {
      if (key.address.ipv4() == 0 || key.address.ipv4() == 0xffffffffu) {
        return std::unexpected("invalid neighbor address");
      }
    } else {
      const utils::Ipv6Address address = key.address.ipv6();
      if (address.IsUnspecified() || address.IsMulticast()) {
        return std::unexpected("invalid neighbor address");
      }
    }
    if (Owns(iface->second, key.address)) {
      return std::unexpected("an interface cannot be its own neighbor");
    }
    if (SourceFor(iface->second, key.address) == nullptr) {
      return std::unexpected(key.address.is_ipv4()
                                 ? "the interface has no IPv4 address to solicit from"
                                 : "the interface has no IPv6 address to solicit from");
    }
    return {};
  }

  // Binds next hop `hop` to neighbor `key` and starts resolving it. Returns
  // the next hop as it should be published now (the neighbor's current state
  // if it is already known, incomplete otherwise). Rebinding a hop moves it;
  // it does not restart an existing neighbor's resolution.
  std::expected<NextHop, std::string> Bind(NextHopId hop, Key key,
                                            uint64_t now) {
    if (auto valid = CheckBind(key); !valid) {
      return std::unexpected(valid.error());
    }
    auto iface = interfaces_.find(key.interface);
    const bool key_exists = entries_.contains(key);
    const std::optional<NeighborTable::Bound> previous =
        neighbors_.BindingOf(hop);
    // A rebind that takes the hop off its only neighbor releases that
    // neighbor's entry (and timer), so it may take the new key even when the
    // table is full.
    const bool releases_entry =
        !key_exists && previous && previous->key != key &&
        entries_.contains(previous->key) &&
        neighbors_.BindingCount(previous->key) == 1;
    if (!key_exists && entries_.size() >= capacity_ && !releases_entry) {
      return std::unexpected("neighbor capacity exhausted");
    }

    // What a rebind would drop, kept until the new timer is armed so a
    // failure can put both the binding and the timer back.
    std::optional<Key> released_key;
    Entry released_entry;
    std::optional<uint64_t> released_deadline;
    if (releases_entry) {
      const auto old = entries_.find(previous->key);
      released_key = previous->key;
      released_entry = old->second;
      released_deadline = wheel_->DeadlineOf(released_entry.timer);
    }

    auto [entry, inserted] = entries_.try_emplace(key);
    NextHop initial;
    try {
      initial = neighbors_.Bind(hop, key, iface->second.mac);
    } catch (...) {
      if (inserted) {
        entries_.erase(entry);
      }
      throw;
    }
    if (!inserted) {
      DropIfUnbound(previous, key);
      return initial;
    }

    // Free the released neighbor's timer before arming the new one, so a
    // full wheel still has room for the moving hop.
    if (released_key) {
      auto stale = entries_.find(*released_key);
      if (stale != entries_.end()) {
        Release(stale);
      }
    }

    entry->second.epoch = iface->second.epoch;
    bool armed = true;
    const auto saved = deferred_entries_.find(key);
    if (saved != deferred_entries_.end()) {
      // Released earlier in this batch (DeferForgetting): it continues as if
      // it had never been, with its reconfirmation, probe count and deadline.
      const Entry &was = saved->second.entry;
      entry->second.state = was.state;
      entry->second.probes = was.probes;
      entry->second.reconfirming = was.reconfirming;
      entry->second.serial = was.serial;
      if (const std::optional<uint64_t> &deadline = saved->second.deadline;
          deadline.has_value()) {
        armed = ArmAt(key, entry->second, std::max(*deadline, now));
      }
    } else {
      const auto learned = neighbors_.Find(key);
      const NeighborState state =
          learned ? learned->state : NeighborState::kIncomplete;
      entry->second.state = state;
      entry->second.probes = 0;
      entry->second.reconfirming = false;
      // A neighbor that is already known continues from that state; a fresh
      // one solicits. The two "no timer" configurations (aging and retry
      // disabled) leave the entry in its current state.
      if (state == NeighborState::kResolved) {
        armed = timing_.reachable_lifetime == 0 ||
                Arm(key, entry->second, now, timing_.reachable_lifetime);
      } else if (state == NeighborState::kUnreachable) {
        armed = timing_.unreachable_retry == 0 ||
                Arm(key, entry->second, now, timing_.unreachable_retry);
      } else {
        armed = Arm(key, entry->second, now, timing_.first_probe);
      }
    }
    if (!armed) {
      // The wheel refused the new timer. Put the hop back where it was (a
      // prior binding, or no binding at all) and restore the released
      // neighbor's timer, so a refused rebind changes nothing.
      if (previous) {
        try {
          (void)neighbors_.Bind(hop, previous->key, previous->src_mac);
        } catch (...) {
          // The table cannot usually fail here (the maps already hold the
          // nodes); leaving the hop unbound is the safe fallback.
          neighbors_.Unbind(hop);
        }
      } else {
        neighbors_.Unbind(hop);
      }
      entries_.erase(entry);
      if (released_key) {
        auto [restored, fresh] =
            entries_.try_emplace(*released_key, released_entry);
        if (fresh) {
          restored->second.timer = dataplane::kNoExpiry;
          if (released_deadline) {
            ArmAt(*released_key, restored->second, *released_deadline);
          }
        }
      }
      return std::unexpected("timer capacity exhausted");
    }
    if (saved != deferred_entries_.end()) {
      deferred_entries_.erase(saved);
    }
    DropIfUnbound(previous, key);
    return initial;
  }

  // Forgets `hop`'s binding. A neighbor no next hop names any more stops
  // being resolved and is forgotten -- its timer, its solicitations and what
  // was learned about it -- so the table holds only bound neighbors and a
  // later binding of the same address starts incomplete and solicits. Inside
  // DeferForgetting() only its timer and capacity go at once; what was learned
  // waits for ForgetUnbound().
  void Unbind(NextHopId hop) {
    const std::optional<NeighborTable::Bound> binding =
        neighbors_.BindingOf(hop);
    neighbors_.Unbind(hop);
    if (!binding || neighbors_.HasBindings(binding->key)) {
      return;
    }
    auto entry = entries_.find(binding->key);
    if (entry != entries_.end()) {
      Release(entry);
    }
    if (!defer_forgetting_) {
      neighbors_.Forget(binding->key);
    }
  }

  // Starts a batch of Bind and Unbind calls that moves bindings between next
  // hops. Until ForgetUnbound(), a neighbor whose last binding goes releases
  // its timer and its room at once but keeps what was learned and where its
  // resolution stood (reconfirming, probes sent, the timer's deadline), so a
  // Bind later in the batch takes it up exactly where it was, whatever order
  // the batch runs in.
  void DeferForgetting() noexcept { defer_forgetting_ = true; }
  // Ends the batch: forgets every neighbor no next hop is bound to.
  void ForgetUnbound() noexcept {
    defer_forgetting_ = false;
    deferred_entries_.clear();
    neighbors_.ForgetUnbound();
  }

  // The neighbor `hop` is bound to, if any.
  std::optional<Key> BindingOf(NextHopId hop) const {
    const std::optional<NeighborTable::Bound> binding = neighbors_.BindingOf(hop);
    if (!binding) {
      return std::nullopt;
    }
    return binding->key;
  }

  // -- events -----------------------------------------------------------------

  // Feeds one ARP frame received on `ingress` to the resolver. Returns the
  // neighbor changes it caused (empty for a frame that changes nothing).
  //
  // A reply whose target is one of the interface's addresses -- unicast to
  // the interface's MAC, or broadcast -- answers a solicitation: the neighbor
  // is resolved on the sender's MAC and its reachable lifetime restarts.
  //
  // Any other ARP frame from a bound neighbor is RFC 826's merge: a request
  // (for any target, gratuitous ones included) or a broadcast reply naming
  // another target says where the sender is now. (A unicast reply naming
  // another target answers nothing we asked, and is ignored.) A sender that
  // is already resolved on that MAC changes nothing. Otherwise the new MAC is
  // published as resolved at once -- so a moved neighbor, a failed-over
  // address, is followed without waiting for aging -- and the neighbor is
  // reconfirmed on the next Poll: it is solicited, and fails to kUnreachable
  // if it does not answer. Answering requests stays the local responder's
  // (AnswerArpRequest in l3_packet.h).
  //
  // Neither ever creates a neighbor: a sender no Bind named is ignored, so
  // unsolicited ARP cannot populate the table. Both need the Ethernet source
  // to match the ARP sender, a usable unicast sender MAC that is not the
  // interface's own, an Ethernet destination that is the interface's MAC or
  // broadcast, and the interface generation the entry was bound under.
  Progress OnArp(uint64_t now, dataplane::InterfaceId ingress,
                 std::span<const uint8_t> frame) {
    Progress out = NewProgress();
    auto iface = interfaces_.find(ingress);
    if (iface == interfaces_.end()) {
      return out;
    }
    const std::optional<ArpMessage> arp = ParseArp(frame);
    if (!arp || (arp->opcode != utils::Arp::kReply &&
                 arp->opcode != utils::Arp::kRequest)) {
      return out;
    }
    utils::Ethernet eth;
    std::memcpy(&eth, frame.data(), sizeof(eth));
    if ((eth.dst_addr != iface->second.mac && !eth.dst_addr.IsBroadcast()) ||
        eth.src_addr != arp->sender_mac) {
      return out;
    }
    const utils::Ethernet::Address &mac = arp->sender_mac;
    if (mac.IsZero() || mac.IsBroadcast() ||
        (mac.bytes[0] & uint8_t{0x01}) != 0 || mac == iface->second.mac) {
      return out;
    }
    const Key key{ingress, arp->sender_ip.value()};
    auto entry = entries_.find(key);
    if (entry == entries_.end() ||
        entry->second.epoch != iface->second.epoch) {
      return out;
    }
    const bool reply = arp->opcode == utils::Arp::kReply;
    const bool answer =
        reply &&
        Owns(iface->second, NeighborAddress::Ipv4(arp->target_ip.value()));
    if (reply && !answer && !eth.dst_addr.IsBroadcast()) {
      // A reply sent to us for an address that is not ours -- one in flight
      // when the interface's address moved -- answers nothing we asked.
      return out;
    }
    if (!answer) {
      const auto current = neighbors_.Find(key);
      if (current && current->state == NeighborState::kResolved &&
          current->mac == mac) {
        return out;  // nothing new: the neighbor is where it was
      }
    }
    // Stage every fallible allocation first: the table update and the output
    // space. The resolved state is then committed only with its timer in
    // place: a live timer is rescheduled in place, and a refused arming (the
    // wheel has no usable node left) leaves the entry, its timer and the table
    // as they were and the frame is ignored. An answer's timer is the
    // reachable lifetime (zero: do not age); a merge's is due at once, which
    // starts its reconfirmation on the next Poll.
    NeighborTable::Prepared prepared =
        neighbors_.Prepare(key, NeighborState::kResolved, mac);
    out.updates.reserve(prepared.updates.size());
    if (!answer) {
      if (!Arm(key, entry->second, now, 0)) {
        return out;
      }
    } else if (timing_.reachable_lifetime != 0) {
      if (!Arm(key, entry->second, now, timing_.reachable_lifetime)) {
        return out;
      }
    } else {
      Disarm(entry->second);
    }
    neighbors_.Commit(prepared);
    entry->second.state = NeighborState::kResolved;
    entry->second.probes = 0;
    entry->second.reconfirming = false;
    AppendUpdates(std::move(prepared.updates), out);
    return out;
  }

  // Feeds one IPv6 Neighbor Discovery frame received on `ingress` to the
  // resolver. Only Neighbor Advertisements are learned from; Neighbor
  // Solicitations are the local responder's (AnswerNeighborSolicitation in
  // l3_packet.h). The advertisement names its neighbor by target address,
  // not by source: a non-proxy advertisement has source == target, which this
  // library requires, and its target link-layer option (or the Ethernet
  // source when the option is absent) is the address to publish. A solicited
  // advertisement may override a resolved MAC; an unsolicited one may not
  // (`override` is the O flag). A reply never creates an entry.
  Progress OnNd(uint64_t now, dataplane::InterfaceId ingress,
                std::span<const uint8_t> frame) {
    Progress out = NewProgress();
    auto iface = interfaces_.find(ingress);
    if (iface == interfaces_.end()) {
      return out;
    }
    const std::optional<NdMessage> advertisement =
        ParseNeighborDiscovery(frame);
    if (!advertisement ||
        advertisement->type != NdType::kNeighborAdvertisement) {
      return out;
    }
    utils::Ethernet eth;
    std::memcpy(&eth, frame.data(), sizeof(eth));
    // A solicited advertisement is unicast to our current address and MAC;
    // an unsolicited one (RFC 4861's MAC-change notification) goes to
    // all-nodes. Anything else -- an old own address, another group, another
    // interface's MAC -- is not this generation's neighbor.
    const bool to_us =
        eth.dst_addr == iface->second.mac &&
        Owns(iface->second, NeighborAddress::Ipv6(advertisement->destination_ip));
    const bool to_all_nodes =
        eth.dst_addr == AllNodesMac() &&
        advertisement->destination_ip == AllNodesAddress();
    if ((!to_us && !to_all_nodes) ||
        advertisement->source_ip != advertisement->target_ip ||
        advertisement->source_ip.IsLoopback()) {
      // Not for this interface, a proxied advertisement (unsupported), or a
      // source that is not a link address.
      return out;
    }
    const utils::Ethernet::Address mac =
        advertisement->target_ll.value_or(advertisement->source_mac);
    if (mac.IsZero() || mac.IsBroadcast() ||
        (static_cast<unsigned>(mac.bytes[0]) & 1u) != 0 ||
        mac == iface->second.mac) {
      return out;
    }
    const Key key{ingress, advertisement->target_ip};
    auto entry = entries_.find(key);
    if (entry == entries_.end() ||
        entry->second.epoch != iface->second.epoch) {
      return out;
    }
    const auto current = neighbors_.Find(key);
    if (current && current->state == NeighborState::kResolved &&
        !advertisement->override && current->mac != mac) {
      // RFC 4861 override semantics: a resolved neighbor keeps its MAC unless
      // the advertisement says to override it.
      return out;
    }
    // Same staging as OnArp: the table update and output space first, then the
    // aging timer in place (Reschedule), and only with the timer in place is
    // the resolved state committed. A refused arming leaves the entry, its
    // timer and the table as they were and ignores the advertisement.
    NeighborTable::Prepared prepared =
        neighbors_.Prepare(key, NeighborState::kResolved, mac);
    out.updates.reserve(prepared.updates.size());
    if (timing_.reachable_lifetime != 0) {
      if (!Arm(key, entry->second, now, timing_.reachable_lifetime)) {
        return out;
      }
    } else {
      Disarm(entry->second);
    }
    neighbors_.Commit(prepared);
    entry->second.state = NeighborState::kResolved;
    entry->second.probes = 0;
    entry->second.reconfirming = false;
    AppendUpdates(std::move(prepared.updates), out);
    return out;
  }

  // Advances the resolver's timers by at most `budget` wheel units, emitting
  // the solicitations and transitions that are due. Call it between batches
  // on the resolver's thread, with the scheduler's cached time and a bounded
  // budget, as ExpiryWheel::Poll does.
  //
  // Two phases. The wheel spends the due nodes while collecting them; every
  // allocation the transitions need (their next-hop update vectors and the
  // output space) happens in the planning phase, before any table, timer or
  // resolver state changes. Only when all of that succeeded are the plans
  // committed, which allocates nothing. If planning throws, every due entry
  // is re-armed at the deadline it was collected for, so no timer is lost and
  // no update is partially applied; the retry (the next Poll) sees the same
  // work.
  Progress Poll(uint64_t now, size_t budget) {
    Progress out = NewProgress();  // before consuming, so its space is staged
    due_.clear();
    wheel_->Poll(now, budget, [this](const TimerPayload &payload) noexcept {
      if (due_.size() < due_.capacity()) {
        due_.push_back(payload);
      }
    });
    std::vector<Plan> plans;
    try {
      plans.reserve(due_.size());
      size_t update_count = 0;
      for (const TimerPayload &payload : due_) {
        auto entry = entries_.find(payload.key);
        if (entry == entries_.end() ||
            entry->second.serial != payload.serial) {
          continue;  // a record that outlived its entry
        }
        plans.emplace_back();
        Plan &plan = plans.back();
        plan.key = payload.key;
        auto iface = interfaces_.find(payload.key.interface);
        if (iface == interfaces_.end() ||
            iface->second.epoch != entry->second.epoch) {
          // The interface left or moved after the timer was armed: the entry
          // goes, the table's newer state is not ours to change.
          plan.remove = true;
          continue;
        }
        PlanDue(plan, entry->second, now);
        update_count +=
            plan.prepared.updates.size() + plan.fail_closed.updates.size();
      }
      out.updates.reserve(update_count);
    } catch (...) {
      RecoverDue();
      throw;
    }

    // Commit: no allocation, and nothing above changed yet.
    for (Plan &plan : plans) {
      auto entry = entries_.find(plan.key);
      if (entry == entries_.end()) {
        continue;
      }
      if (plan.remove) {
        Disarm(entry->second);
        entries_.erase(entry);
        continue;
      }
      // The wheel already spent the timer this plan was collected for.
      entry->second.timer = dataplane::kNoExpiry;
      if (plan.prepared.changed) {
        neighbors_.Commit(plan.prepared);
      }
      entry->second.state = plan.state;
      entry->second.probes = plan.probes;
      entry->second.reconfirming = plan.reconfirming;
      AppendUpdates(std::move(plan.prepared.updates), out);
      if (plan.solicit) {
        auto iface = interfaces_.find(plan.key.interface);
        if (iface != interfaces_.end() && !iface->second.mac.IsZero()) {
          Solicitation solicitation;
          solicitation.egress = plan.key.interface;
          size_t wrote = 0;
          const NeighborAddress *source =
              SourceFor(iface->second, plan.key.address);
          if (source == nullptr) {
            // No address of the neighbor's family: nothing to send from.
          } else if (plan.key.address.is_ipv4()) {
            wrote = BuildArpRequest(iface->second.mac,
                                    utils::be32_t(source->ipv4()),
                                    utils::be32_t(plan.key.address.ipv4()),
                                    solicitation.frame);
          } else {
            wrote = BuildNeighborSolicitation(
                iface->second.mac, source->ipv6(),
                plan.key.address.ipv6(), solicitation.frame);
          }
          if (wrote != 0) {
            solicitation.length = wrote;
            out.solicitations.push_back(solicitation);
          }
        }
      }
      if (plan.arm) {
        if (!ArmAt(plan.key, entry->second, plan.deadline)) {
          // The wheel refused a new node for a timer the entry needs: forget
          // the entry rather than keep a neighbor that nothing will age,
          // retry or recover. An incomplete or unreachable plan state already
          // took the MAC out of service; a reconfirming neighbor still has
          // it, so it fails closed with the transition planned for that.
          if (plan.fail_closed.changed) {
            neighbors_.Commit(plan.fail_closed);
            AppendUpdates(std::move(plan.fail_closed.updates), out);
          }
          Disarm(entry->second);
          entries_.erase(entry);
        }
      }
    }
    return out;
  }

  // -- inspection -------------------------------------------------------------

  std::optional<NeighborTable::Neighbor> Find(Key key) const {
    return neighbors_.Find(key);
  }
  size_t size() const noexcept { return entries_.size(); }
  const NeighborTable &table() const noexcept { return neighbors_; }
  const Timing &timing() const noexcept { return timing_; }
  // Armed timers: at most one per entry.
  size_t armed() const noexcept { return wheel_->size(); }
  // The interface's current generation, or zero for an unknown interface.
  uint32_t interface_epoch(dataplane::InterfaceId id) const {
    auto it = interfaces_.find(id);
    return it == interfaces_.end() ? 0 : it->second.epoch;
  }

  // What the resolver knows about one neighbor, for a state view: the table's
  // state and MAC, whether a resolved neighbor is being reconfirmed, and the
  // solicitations sent in the current attempt. Empty for a key no Bind named.
  struct Status {
    NeighborState state = NeighborState::kIncomplete;
    bool reconfirming = false;
    uint32_t probes = 0;
    utils::Ethernet::Address mac;
  };
  std::optional<Status> StatusOf(Key key) const {
    const auto neighbor = neighbors_.Find(key);
    if (!neighbor) {
      return std::nullopt;
    }
    Status status;
    status.state = neighbor->state;
    status.mac = neighbor->mac;
    auto entry = entries_.find(key);
    if (entry != entries_.end()) {
      status.reconfirming = entry->second.reconfirming;
      status.probes = entry->second.probes;
    }
    return status;
  }

 private:
  // The ExpiryWheel payload. The serial names one arming of one entry: a due
  // record is acted on only if the entry still carries it. The deadline lets
  // Poll re-arm every due entry at its own deadline when a later allocation
  // in the same poll is refused: the wheel spent the node, but the work is
  // not lost.
  struct TimerPayload {
    Key key;
    uint32_t serial;
    uint64_t deadline;
  };

  struct Entry {
    uint32_t epoch = 0;
    NeighborState state = NeighborState::kIncomplete;
    uint32_t probes = 0;  // solicitations sent in the current attempt
    // Resolved, its reachable lifetime over, and being solicited again: it
    // keeps its MAC until the attempt fails (PlanDue).
    bool reconfirming = false;
    uint32_t serial = 0;  // this entry's current arming
    dataplane::ExpiryHandle timer = dataplane::kNoExpiry;
  };

  struct StoredInterface {
    utils::Ethernet::Address mac;
    std::vector<InterfaceAddress> addresses;
    uint32_t epoch;
  };

  // Whether `address` is one of the interface's own.
  static bool Owns(const StoredInterface &iface, const NeighborAddress &address) {
    for (const InterfaceAddress &own : iface.addresses) {
      if (own.address == address) {
        return true;
      }
    }
    return false;
  }

  // The address a solicitation for `target` leaves from: the first whose
  // prefix holds it, else the first of its family; null when the interface
  // has no address of that family.
  static const NeighborAddress *SourceFor(const StoredInterface &iface,
                                          const NeighborAddress &target) {
    const NeighborAddress *first = nullptr;
    for (const InterfaceAddress &own : iface.addresses) {
      if (own.address.family != target.family) {
        continue;
      }
      if (own.Contains(target)) {
        return &own.address;
      }
      if (first == nullptr) {
        first = &own.address;
      }
    }
    return first;
  }

  NeighborResolver(const Config &config,
                   std::unique_ptr<dataplane::ExpiryWheel<TimerPayload>> wheel)
      : wheel_(std::move(wheel)),
        capacity_(config.capacity),
        timing_(config.timing) {
    if (timing_.max_probe_interval < timing_.probe_interval) {
      timing_.max_probe_interval = timing_.probe_interval;
    }
    due_.reserve(config.capacity);
  }

  Progress NewProgress() const {
    Progress out;
    out.solicitations.reserve(capacity_);
    return out;
  }

  static void AppendUpdates(NeighborTable::Updates updates, Progress &out) {
    out.updates.insert(out.updates.end(),
                       std::make_move_iterator(updates.begin()),
                       std::make_move_iterator(updates.end()));
  }

  // The backoff before the next solicitation, given the probes already sent.
  uint64_t RetryDelay(uint32_t probes) const {
    uint64_t delay = timing_.probe_interval;
    for (uint32_t i = 1; i < probes && delay < timing_.max_probe_interval;
         i++) {
      if (delay > (std::numeric_limits<uint64_t>::max() >> uint64_t{1})) {
        return timing_.max_probe_interval;
      }
      delay *= 2;
    }
    return delay < timing_.max_probe_interval ? delay
                                              : timing_.max_probe_interval;
  }

  // Arms the entry's one timer at an absolute deadline. A live timer is
  // rescheduled in place (same node, new deadline and payload): re-arming on a
  // reply therefore spends no wheel capacity and the node's generation never
  // advances, so a steady reply stream cannot exhaust the wheel. A new node is
  // needed only when the entry has no live timer, and then a refusal is
  // returned to the caller.
  bool ArmAt(const Key &key, Entry &entry, uint64_t deadline) {
    entry.serial++;
    const TimerPayload payload{key, entry.serial, deadline};
    if (entry.timer != dataplane::kNoExpiry &&
        wheel_->Reschedule(entry.timer, deadline, payload)) {
      return true;
    }
    if (entry.timer != dataplane::kNoExpiry) {
      wheel_->Cancel(entry.timer);
      entry.timer = dataplane::kNoExpiry;
    }
    const dataplane::ExpiryHandle handle = wheel_->Schedule(deadline, payload);
    if (handle == dataplane::kNoExpiry) {
      entry.serial--;
      return false;
    }
    entry.timer = handle;
    return true;
  }

  // Arms the entry's one timer for `now + delay`, clamped by the wheel's
  // After() so a very long lifetime cannot wrap into the past.
  bool Arm(const Key &key, Entry &entry, uint64_t now, uint64_t delay) {
    return ArmAt(key, entry,
                 dataplane::ExpiryWheel<TimerPayload>::After(now, delay));
  }

  // One due timer's planned transition. Planning allocates (the table update
  // vector); committing changes the table, the entry and the wheel and does
  // not.
  struct Plan {
    Key key{};
    bool remove = false;
    NeighborState state = NeighborState::kIncomplete;
    uint32_t probes = 0;
    bool reconfirming = false;
    bool solicit = false;
    bool arm = false;
    uint64_t deadline = 0;
    NeighborTable::Prepared prepared;
    // For a reconfirming plan: the unreachable transition to commit instead
    // if its timer cannot be armed (Poll), staged here so that commit does
    // not allocate.
    NeighborTable::Prepared fail_closed;
  };

  // What firing `entry`'s timer means, without changing anything. A resolved
  // neighbor whose lifetime ran out starts reconfirming: it stays resolved,
  // MAC included, and is solicited again. An unreachable neighbor's retry
  // solicits again from kIncomplete. An attempt that has sent max_probes fails
  // into kUnreachable (from reconfirming too); otherwise the next solicitation
  // is fired. The plan's table transition is prepared here.
  void PlanDue(Plan &plan, const Entry &entry, uint64_t now) {
    NeighborState state = entry.state;
    uint32_t probes = entry.probes;
    bool reconfirming = entry.reconfirming;
    if (state == NeighborState::kResolved && !reconfirming) {
      reconfirming = true;  // aging: keep the MAC, solicit again
      probes = 0;
    } else if (state == NeighborState::kUnreachable) {
      state = NeighborState::kIncomplete;  // the retry of a failed attempt
      probes = 0;
    }
    if (probes >= timing_.max_probes) {
      state = NeighborState::kUnreachable;
      probes = 0;
      reconfirming = false;
      plan.arm = timing_.unreachable_retry != 0;
      plan.deadline = dataplane::ExpiryWheel<TimerPayload>::After(
          now, timing_.unreachable_retry);
    } else {
      probes++;
      plan.solicit = true;
      plan.arm = true;
      plan.deadline = dataplane::ExpiryWheel<TimerPayload>::After(
          now, RetryDelay(probes));
    }
    plan.state = state;
    plan.probes = probes;
    plan.reconfirming = reconfirming;
    if (state != entry.state) {
      plan.prepared =
          neighbors_.Prepare(plan.key, state, utils::Ethernet::Address{});
    }
    if (reconfirming) {
      plan.fail_closed = neighbors_.Prepare(
          plan.key, NeighborState::kUnreachable, utils::Ethernet::Address{});
    }
  }

  // The wheel already spent these nodes, but planning refused an allocation:
  // re-arm every still-current entry at the deadline its record carried, so
  // nothing is lost and the next Poll retries the work.
  void RecoverDue() noexcept {
    for (const TimerPayload &payload : due_) {
      auto entry = entries_.find(payload.key);
      if (entry == entries_.end() ||
          entry->second.serial != payload.serial) {
        continue;
      }
      entry->second.timer = dataplane::kNoExpiry;
      if (!ArmAt(payload.key, entry->second, payload.deadline)) {
        // Recovery cannot restore this timer; the entry would otherwise stay
        // without one forever, so it leaves the resolver with the table's
        // last committed state.
        Disarm(entry->second);
        entries_.erase(entry);
      }
    }
  }

  void Disarm(Entry &entry) {
    if (entry.timer != dataplane::kNoExpiry) {
      wheel_->Cancel(entry.timer);
      entry.timer = dataplane::kNoExpiry;
    }
    entry.serial++;
  }

  // Stops resolving the neighbor `it` names and frees its room. Inside
  // DeferForgetting() the entry and its deadline are kept aside for a Bind
  // later in the batch; if that record cannot be allocated, the neighbor is
  // taken up from what was learned instead, as outside a batch.
  void Release(std::map<Key, Entry>::iterator it) {
    const std::optional<uint64_t> deadline = wheel_->DeadlineOf(it->second.timer);
    Disarm(it->second);
    if (defer_forgetting_) {
      try {
        deferred_entries_.insert_or_assign(it->first, Deferred{it->second, deadline});
      } catch (const std::bad_alloc &) {
        // No room for the record: Bind takes the neighbor up from what was
        // learned, as outside a batch.
        deferred_entries_.erase(it->first);
      }
    }
    entries_.erase(it);
  }

  // The neighbor's entry is no longer named by any hop: stop resolving it and
  // forget it.
  void DropIfUnbound(const std::optional<NeighborTable::Bound> &previous,
                     const Key &key) {
    if (!previous || previous->key == key ||
        neighbors_.HasBindings(previous->key)) {
      return;
    }
    auto stale = entries_.find(previous->key);
    if (stale != entries_.end()) {
      Release(stale);
    }
    if (!defer_forgetting_) {
      neighbors_.Forget(previous->key);
    }
  }

  std::unique_ptr<dataplane::ExpiryWheel<TimerPayload>> wheel_;
  const size_t capacity_;
  Timing timing_;
  NeighborTable neighbors_;
  std::map<Key, Entry> entries_;
  // Inside DeferForgetting(): an unbound neighbor keeps what was learned.
  bool defer_forgetting_ = false;
  // Inside DeferForgetting(): the entries released in the batch, with their
  // timers' deadlines (none when unarmed), for a later Bind to resume.
  struct Deferred {
    Entry entry;
    std::optional<uint64_t> deadline;
  };
  std::map<Key, Deferred> deferred_entries_;
  std::map<dataplane::InterfaceId, StoredInterface> interfaces_;
  // Poll's collection of due records. Pre-sized in Create so the wheel
  // callback never allocates.
  std::vector<TimerPayload> due_;
};

}  // namespace bess::route

#endif  // BESS_ROUTE_NEIGHBOR_RESOLVER_H_
