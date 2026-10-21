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

// IPv4 neighbor resolution over Ethernet (ARP): solicitation with retries and
// backoff, learning, aging, MAC changes, failure and recovery (M15, D-175).
//
// The resolver is the event source for the neighbor table
// (`neighbor_table.h`): it owns one neighbor entry per (interface, IPv4
// address), created by Bind, and moves it through
//
//   kIncomplete --reply--> kResolved --aging--> kIncomplete (re-solicit)
//        |                                   |
//        +--max probes--> kUnreachable --retry--> kIncomplete
//
// Every transition calls NeighborTable::Update and returns the next hops to
// republish, so the caller still publishes adjacency with the existing
// mechanism -- Router::SetNextHop through Publish(), or one transaction built
// from NextHopOps(). The library sends and parses packets but writes no
// packet and no route: the caller owns the graph, the gates, the buffers and
// the policy for a frame whose next hop is unresolved.
//
// Identity. A neighbor is named by (dataplane::InterfaceId, IPv4 address), the
// same key as NeighborTable. The interface also has an identity of its own --
// its MAC and its own address -- and every entry remembers the interface
// generation it was created under. A reply is accepted only when all of these
// hold:
//   - the frame arrives on the interface the entry belongs to;
//   - the entry's interface generation is still current;
//   - the reply's Ethernet source and ARP sender match, and the frame is
//     addressed to the interface's MAC;
//   - the reply's sender address is the entry's address;
//   - the reply's target address is the interface's current own address;
//   - the sender's MAC is a usable unicast address.
// A stale reply -- one in flight when the interface's address moved, one that
// arrives on another interface, or one for a neighbor that was unbound and
// rebound -- is ignored rather than applied to a newer entry. A reply never
// creates a neighbor: only Bind does, so an unsolicited frame cannot populate
// the table.
//
// Timers. The resolver reuses the worker-owned ExpiryWheel
// (`dataplane/expiry_wheel.h`) instead of keeping a second timer wheel. Each
// neighbor has at most one armed timer, its capacity is the configured
// neighbor capacity, and a payload carries the entry's arming serial so a
// record that outlives its entry cannot act on a newer one. The resolver is
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
// IPv6 ND is not implemented yet; the state machine and the key are ready for
// another address family over the same entries, and the packet helpers are
// address-family specific.
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
    // How long a resolved neighbor stays usable before it is solicited again.
    // Zero disables aging: a resolved neighbor stays resolved until a reply
    // changes it or it is unbound.
    uint64_t reachable_lifetime = 30000;
    // How long an unreachable neighbor waits before another attempt. Zero
    // means no further attempt: it stays unreachable until a reply arrives or
    // the caller removes and rebinds it.
    uint64_t unreachable_retry = 60000;
  };

  struct Config {
    // Bound on neighbors and therefore on armed timers. Every entry has at
    // most one timer, so Create() allocates exactly this many wheel nodes.
    size_t capacity = 1024;
    // ExpiryWheel's granularity shift (0: one tick).
    unsigned granularity_shift = 0;
    Timing timing;
  };

  // The identity of an interface for resolution: what it sends from (MAC and
  // own address) and how it is named. Addresses are host order, as in Key;
  // the packet helpers convert at the wire boundary.
  struct Interface {
    dataplane::InterfaceId id;
    utils::Ethernet::Address mac;
    uint32_t ipv4;  // its own address; zero: no address, cannot solicit
  };

  // One ARP request to put on the wire. `frame` is a complete Ethernet frame
  // (kArpFrameSize bytes); a port that needs the 60-byte minimum pads it.
  struct Solicitation {
    dataplane::InterfaceId egress;
    std::array<uint8_t, kArpFrameSize> frame;
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

  NeighborResolver(const NeighborResolver &) = delete;
  NeighborResolver &operator=(const NeighborResolver &) = delete;

  // -- control side -----------------------------------------------------------

  // Adds an interface or replaces its identity. A changed MAC or address
  // starts a new interface generation: every neighbor on it is marked
  // incomplete, its pending solicitation is cancelled, and resolution starts
  // again from the new identity. The same identity again returns an empty
  // Progress. `now` is the clock for the new first probes.
  std::expected<Progress, std::string> ConfigureInterface(const Interface &iface,
                                                          uint64_t now) {
    auto it = interfaces_.find(iface.id);
    if (it != interfaces_.end() && it->second.mac == iface.mac &&
        it->second.ipv4 == iface.ipv4) {
      return Progress{};
    }
    uint32_t epoch = it == interfaces_.end() ? 1 : it->second.epoch + 1;
    if (epoch == 0) {
      epoch = 1;  // wrapped: no entry can still name the old generation
    }
    interfaces_[iface.id] = StoredInterface{iface.mac, iface.ipv4, epoch};
    return ResetInterfaceEntries(iface.id, now);
  }

  // Removes the interface: every neighbor on it becomes unreachable and its
  // binding stays in the neighbor table (the next hops remain readable), but
  // the resolver forgets its state and timers. A later ConfigureInterface
  // does not revive those neighbors; Bind them again.
  Progress RemoveInterface(dataplane::InterfaceId id) {
    interfaces_.erase(id);
    Progress out = NewProgress();
    const std::vector<Key> keys = neighbors_.KeysForInterface(id);
    for (const Key &key : keys) {
      // Allocations before the table or the resolver changes.
      out.updates.reserve(out.updates.size() + neighbors_.BindingCount(key));
      NeighborTable::Updates updates = neighbors_.Update(
          key, NeighborState::kUnreachable, utils::Ethernet::Address{});
      auto entry = entries_.find(key);
      if (entry != entries_.end()) {
        Disarm(entry->second);
        entries_.erase(entry);
      }
      AppendUpdates(std::move(updates), out);
    }
    return out;
  }

  // Binds next hop `hop` to neighbor `key` and starts resolving it. Returns
  // the next hop as it should be published now (the neighbor's current state
  // if it is already known, incomplete otherwise). Rebinding a hop moves it;
  // it does not restart an existing neighbor's resolution.
  std::expected<NextHop, std::string> Bind(NextHopId hop, Key key,
                                            uint64_t now) {
    auto iface = interfaces_.find(key.interface);
    if (iface == interfaces_.end()) {
      return std::unexpected("unknown interface");
    }
    if (key.ipv4 == 0 || key.ipv4 == 0xffffffffu) {
      return std::unexpected("invalid neighbor address");
    }
    if (key.ipv4 == iface->second.ipv4) {
      return std::unexpected("an interface cannot be its own neighbor");
    }
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
        Disarm(stale->second);
        entries_.erase(stale);
      }
    }

    entry->second.epoch = iface->second.epoch;
    const auto learned = neighbors_.Find(key);
    const NeighborState state =
        learned ? learned->state : NeighborState::kIncomplete;
    entry->second.state = state;
    entry->second.probes = 0;
    // A neighbor that is already known continues from that state; a fresh
    // one solicits. The two "no timer" configurations (aging and retry
    // disabled) leave the entry in its current state.
    bool armed = true;
    if (state == NeighborState::kResolved) {
      armed = timing_.reachable_lifetime == 0 ||
              Arm(key, entry->second, now, timing_.reachable_lifetime);
    } else if (state == NeighborState::kUnreachable) {
      armed = timing_.unreachable_retry == 0 ||
              Arm(key, entry->second, now, timing_.unreachable_retry);
    } else {
      armed = Arm(key, entry->second, now, timing_.first_probe);
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
    DropIfUnbound(previous, key);
    return initial;
  }

  // Forgets `hop`'s binding. A neighbor no next hop names any more stops
  // being resolved here; a learned entry stays in the neighbor table but has
  // no timers and no solicitations.
  void Unbind(NextHopId hop) {
    const std::optional<NeighborTable::Bound> binding =
        neighbors_.BindingOf(hop);
    neighbors_.Unbind(hop);
    if (!binding || neighbors_.HasBindings(binding->key)) {
      return;
    }
    auto entry = entries_.find(binding->key);
    if (entry != entries_.end()) {
      Disarm(entry->second);
      entries_.erase(entry);
    }
  }

  // -- events -----------------------------------------------------------------

  // Feeds one ARP frame received on `ingress` to the resolver. Only replies
  // are acted on; requests are the local responder's (see AnswerArpRequest in
  // l3_packet.h). Returns the neighbor changes the reply caused (empty for a
  // frame that is not a valid reply for a neighbor of this interface).
  Progress OnArp(uint64_t now, dataplane::InterfaceId ingress,
                 std::span<const uint8_t> frame) {
    Progress out = NewProgress();
    auto iface = interfaces_.find(ingress);
    if (iface == interfaces_.end()) {
      return out;
    }
    const std::optional<ArpMessage> arp = ParseArp(frame);
    if (!arp || arp->opcode != utils::Arp::kReply) {
      return out;
    }
    utils::Ethernet eth;
    std::memcpy(&eth, frame.data(), sizeof(eth));
    if (eth.dst_addr != iface->second.mac ||
        eth.src_addr != arp->sender_mac) {
      return out;
    }
    const Key key{ingress, arp->sender_ip.value()};
    auto entry = entries_.find(key);
    if (entry == entries_.end()) {
      return out;
    }
    if (entry->second.epoch != iface->second.epoch ||
        arp->target_ip.value() != iface->second.ipv4 ||
        arp->sender_mac.IsZero() || arp->sender_mac.IsBroadcast() ||
        (arp->sender_mac.bytes[0] & uint8_t{0x01}) != 0 ||
        arp->sender_mac == iface->second.mac) {
      return out;
    }
    // All fallible work first: the update vector's space, then the table's
    // own update. Only then is the timer cancelled and the resolver state
    // changed, so a refused allocation leaves the entry and its timer as
    // they were.
    out.updates.reserve(neighbors_.BindingCount(key));
    NeighborTable::Updates updates =
        neighbors_.Update(key, NeighborState::kResolved, arp->sender_mac);
    Disarm(entry->second);
    entry->second.state = NeighborState::kResolved;
    entry->second.probes = 0;
    AppendUpdates(std::move(updates), out);
    // A lifetime of zero means "do not age".
    if (timing_.reachable_lifetime != 0) {
      Arm(key, entry->second, now, timing_.reachable_lifetime);
    }
    return out;
  }

  // Advances the resolver's timers by at most `budget` wheel units, emitting
  // the solicitations and transitions that are due. Call it between batches
  // on the resolver's thread, with the scheduler's cached time and a bounded
  // budget, as ExpiryWheel::Poll does.
  Progress Poll(uint64_t now, size_t budget) {
    Progress out = NewProgress();
    due_.clear();
    wheel_->Poll(now, budget, [this](const TimerPayload &payload) noexcept {
      if (due_.size() < due_.capacity()) {
        due_.push_back(payload);
      }
    });
    for (const TimerPayload &payload : due_) {
      auto entry = entries_.find(payload.key);
      if (entry == entries_.end() || entry->second.serial != payload.serial) {
        continue;  // a record that outlived its entry
      }
      entry->second.timer = dataplane::kNoExpiry;
      Fire(payload.key, entry->second, now, out);
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

 private:
  // The ExpiryWheel payload. The serial names one arming of one entry: a due
  // record is acted on only if the entry still carries it.
  struct TimerPayload {
    Key key;
    uint32_t serial;
  };

  struct Entry {
    uint32_t epoch = 0;
    NeighborState state = NeighborState::kIncomplete;
    uint32_t probes = 0;  // solicitations sent in the current attempt
    uint32_t serial = 0;  // this entry's current arming
    dataplane::ExpiryHandle timer = dataplane::kNoExpiry;
  };

  struct StoredInterface {
    utils::Ethernet::Address mac;
    uint32_t ipv4;
    uint32_t epoch;
  };

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

  // Arms the entry's one timer at an absolute deadline.
  bool ArmAt(const Key &key, Entry &entry, uint64_t deadline) {
    if (entry.timer != dataplane::kNoExpiry) {
      wheel_->Cancel(entry.timer);
      entry.timer = dataplane::kNoExpiry;
    }
    entry.serial++;
    const dataplane::ExpiryHandle handle =
        wheel_->Schedule(deadline, TimerPayload{key, entry.serial});
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

  // Records `state` for `key` after reserving the output space first: every
  // allocation this needs happens before the table or the resolver changes,
  // so a refused allocation leaves both as they were and the caller can
  // retry. The returned updates fit in `out` without allocating.
  NeighborTable::Updates StageUpdate(const Key &key, NeighborState state,
                                     const utils::Ethernet::Address &mac,
                                     Progress &out) {
    out.updates.reserve(out.updates.size() + neighbors_.BindingCount(key));
    return neighbors_.Update(key, state, mac);
  }

  void Disarm(Entry &entry) {
    if (entry.timer != dataplane::kNoExpiry) {
      wheel_->Cancel(entry.timer);
      entry.timer = dataplane::kNoExpiry;
    }
    entry.serial++;
  }

  // The neighbor's entry is no longer named by any hop: stop resolving it.
  void DropIfUnbound(const std::optional<NeighborTable::Bound> &previous,
                     const Key &key) {
    if (!previous || previous->key == key ||
        neighbors_.HasBindings(previous->key)) {
      return;
    }
    auto stale = entries_.find(previous->key);
    if (stale != entries_.end()) {
      Disarm(stale->second);
      entries_.erase(stale);
    }
  }

  // A due timer for `key`'s entry.
  void Fire(const Key &key, Entry &entry, uint64_t now, Progress &out) {
    auto iface = interfaces_.find(key.interface);
    if (iface == interfaces_.end() || iface->second.epoch != entry.epoch) {
      // The interface left or moved after the timer was armed: drop the
      // resolver's entry; the table's newer state is not ours to change.
      Disarm(entry);
      entries_.erase(key);
      return;
    }
    if (entry.state == NeighborState::kResolved ||
        entry.state == NeighborState::kUnreachable) {
      // Aging or the retry of a failed attempt: solicit again. A resolved
      // neighbor loses its MAC until a reply arrives (NeighborTable::Make
      // clears it for an incomplete state).
      NeighborTable::Updates updates = StageUpdate(
          key, NeighborState::kIncomplete, utils::Ethernet::Address{}, out);
      entry.state = NeighborState::kIncomplete;
      entry.probes = 0;
      AppendUpdates(std::move(updates), out);
    }

    if (entry.probes >= timing_.max_probes) {
      // The attempt is over: publish failure and wait for the retry interval.
      NeighborTable::Updates updates = StageUpdate(
          key, NeighborState::kUnreachable, utils::Ethernet::Address{}, out);
      entry.state = NeighborState::kUnreachable;
      entry.probes = 0;
      AppendUpdates(std::move(updates), out);
      if (timing_.unreachable_retry != 0) {
        Arm(key, entry, now, timing_.unreachable_retry);
      }
      return;
    }

    entry.probes++;
    if (iface->second.ipv4 != 0 && !iface->second.mac.IsZero()) {
      Solicitation solicitation;
      solicitation.egress = key.interface;
      const size_t wrote =
          BuildArpRequest(iface->second.mac, utils::be32_t(iface->second.ipv4),
                          utils::be32_t(key.ipv4), solicitation.frame);
      if (wrote == kArpFrameSize) {
        out.solicitations.push_back(solicitation);
      }
    }
    // Without a usable source identity the attempt still advances, so a
    // configured-away interface fails cleanly instead of spinning.
    Arm(key, entry, now, RetryDelay(entry.probes));
  }

  // Re-arms every neighbor on `id` from the interface's new identity. It
  // walks the neighbor table, not only the resolver's entries, so learning
  // retained for neighbors that currently have no bound next hop is
  // invalidated too: a later Bind must not reuse a MAC learned under the old
  // interface generation.
  Progress ResetInterfaceEntries(dataplane::InterfaceId id, uint64_t now) {
    Progress out = NewProgress();
    const auto iface = interfaces_.find(id);
    const std::vector<Key> keys = neighbors_.KeysForInterface(id);
    for (const Key &key : keys) {
      // Reserve the output before the fallible table update; the table
      // update itself builds all of its objects before changing anything.
      out.updates.reserve(out.updates.size() + neighbors_.BindingCount(key));
      NeighborTable::Updates updates = neighbors_.Reconfigure(
          key, iface->second.mac, NeighborState::kIncomplete,
          utils::Ethernet::Address{});
      auto entry = entries_.find(key);
      if (entry != entries_.end()) {
        Disarm(entry->second);
        entry->second.epoch = iface->second.epoch;
        entry->second.state = NeighborState::kIncomplete;
        entry->second.probes = 0;
      }
      AppendUpdates(std::move(updates), out);
      if (entry != entries_.end()) {
        Arm(key, entry->second, now, timing_.first_probe);
      }
    }
    return out;
  }

  std::unique_ptr<dataplane::ExpiryWheel<TimerPayload>> wheel_;
  const size_t capacity_;
  Timing timing_;
  NeighborTable neighbors_;
  std::map<Key, Entry> entries_;
  std::map<dataplane::InterfaceId, StoredInterface> interfaces_;
  // Poll's collection of due records. Pre-sized in Create so the wheel
  // callback never allocates.
  std::vector<TimerPayload> due_;
};

}  // namespace bess::route

#endif  // BESS_ROUTE_NEIGHBOR_RESOLVER_H_
