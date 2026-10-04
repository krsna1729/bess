// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONNTRACK_SHARED_CONNTRACK_H_
#define BESS_CONNTRACK_SHARED_CONNTRACK_H_

#include <rte_pause.h>
#include <rte_spinlock.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>

#include "conntrack/conntrack.h"
#include "dataplane/expiry_wheel.h"
#include "flow/shared_flow_table.h"
#include "rcu/rcu_domain.h"

namespace bess::conntrack {

// Connection tracking shared by every worker (TP8, D-081, table_policy.md 5.3):
// the SHARED mode, for traffic whose two directions may reach different
// workers (asymmetric RSS, or no RSS guarantee at all). Same verdicts as
// Conntrack (the same key, state machine and timeouts: ct_internal), on a
// SharedFlowTable:
//
//   lookups      lock-free (the table's RCU-protected directory);
//   a packet of  takes its connection's one-byte lock for the state machine
//   a connection (two workers on one connection serialise there, nowhere
//                else) and stores its new deadline; the wheel is touched only
//                when the deadline moves earlier than the armed timer (a TCP
//                close), under the tracker's lock;
//   creating     holds the tracker's lock (the table's insert and the timer);
//   expiry       whichever worker takes the tracker's lock without waiting:
//                a timer that fires re-arms to the connection's deadline if a
//                packet moved it (the owner-kept deadline, as NAT does), else
//                erases; erased entries are freed after an RCU grace period.
//
// Every worker that calls Track must be a reader of the domain given at
// Create and pass quiescent states (BESS workers do).
template <typename UserData = NoUserData>
  requires std::is_trivially_copyable_v<UserData>
class SharedConntrack {
 public:
  using Tick = uint64_t;

  struct Entry {
    TcpState tcp = TcpState::kNone;
    bool initiator_is_a = true;
    bool replied = false;
    std::atomic<uint8_t> lock{0};  // the state machine's
    std::atomic<Tick> deadline{0};
    std::atomic<Tick> armed{0};  // the wheel timer's deadline (tracker lock)
    dataplane::ExpiryHandle timer{};
    UserData user{};

    Entry() = default;
    Entry(TcpState t, bool initiator, Tick due)
        : tcp(t), initiator_is_a(initiator), deadline(due), armed(due) {}
  };
  struct EntryTraits : flow::DefaultSharedFlowTableTraits {};
  using Table = flow::SharedFlowTable<CtKey, Entry, EntryTraits>;
  using Wheel = dataplane::ExpiryWheel<flow::FlowHandle, Tick>;

  struct Result {
    TrackStatus status = TrackStatus::kInvalid;
    Direction direction = Direction::kOriginal;
    flow::FlowHandle handle{};
    Entry *entry = nullptr;  // valid until this worker's next quiescent state
  };

  enum class CreateError : uint8_t { kInvalidConfig, kOutOfMemory };

  static std::expected<std::unique_ptr<SharedConntrack>, CreateError> Create(
      size_t capacity, rcu::RcuDomain &domain, const TimeoutPolicy &policy = TimeoutPolicy{},
      Tick start = 0, unsigned granularity_shift = 0) {
    auto table = Table::Create(capacity, domain);
    if (!table) {
      return std::unexpected(table.error() == flow::FlowTableError::kOutOfMemory
                                 ? CreateError::kOutOfMemory
                                 : CreateError::kInvalidConfig);
    }
    auto wheel = Wheel::Create(capacity, start, granularity_shift);
    if (!wheel) {
      return std::unexpected(wheel.error() == dataplane::ExpiryError::kOutOfMemory
                                 ? CreateError::kOutOfMemory
                                 : CreateError::kInvalidConfig);
    }
    std::unique_ptr<SharedConntrack> ct(
        new (std::nothrow) SharedConntrack(std::move(*table), std::move(*wheel), policy));
    if (ct == nullptr) {
      return std::unexpected(CreateError::kOutOfMemory);
    }
    return ct;
  }

  // As Conntrack::Track.
  Result Track(std::span<const uint8_t> frame, const ParsedFlowPacket &p, Tick now,
               uint16_t zone = 0, bool may_create = true) noexcept {
    if (!ct_internal::Keyed(p)) {
      return Unkeyed(frame, p, zone);
    }
    const CanonicalKey c = MakeKey(p, zone);
    const flow::FlowHandle h = table_->FindHandle(c.key).handle;
    if (Entry *e = h.id.value() != 0 ? table_->Lookup(h) : nullptr) {
      return Existing(p, c, h, *e, now, may_create);
    }
    return Create(p, c, now, may_create);
  }

  static constexpr size_t kMaxBatch = Table::kMaxBatch;

  // Track for each packet of a batch, in order.
  void TrackBatch(std::span<const std::span<const uint8_t>> frames,
                  std::span<const ParsedFlowPacket> parsed, Tick now, std::span<Result> out,
                  uint16_t zone = 0, bool may_create = true) noexcept {
    for (size_t i = 0; i < frames.size(); i++) {
      out[i] = Track(frames[i], parsed[i], now, zone, may_create);
    }
  }

  // Removes connections past their deadline, at most `budget` units of wheel
  // work; nothing (0) when another worker holds the tracker's lock.
  size_t Expire(Tick now, size_t budget) noexcept {
    if (!rte_spinlock_trylock(&lock_)) {
      return 0;
    }
    (void)table_->Reclaim();  // slots of earlier erases whose grace period passed
    size_t removed = 0;
    (void)wheel_->Poll(now, budget,
                       [this, now, &removed](const flow::FlowHandle &h) noexcept
                           -> std::optional<Tick> {
                         Entry *e = table_->Lookup(h);
                         if (e == nullptr) {
                           return std::nullopt;
                         }
                         Tick due = e->deadline.load(std::memory_order_relaxed);
                         if (static_cast<int64_t>(due - now) > 0) {
                           // Re-arm to the deadline a packet moved it to. A packet
                           // that moves it earlier at the same time stores the
                           // deadline, then reads `armed`; here `armed` is stored,
                           // then the deadline read again: with a full fence on
                           // both sides, one of the two sees the other's store
                           // (Dekker), so the earlier deadline is never lost.
                           e->armed.store(due, std::memory_order_relaxed);
                           std::atomic_thread_fence(std::memory_order_seq_cst);
                           const Tick again = e->deadline.load(std::memory_order_relaxed);
                           if (static_cast<int64_t>(due - again) > 0) {
                             due = again;
                             e->armed.store(due, std::memory_order_relaxed);
                           }
                           if (static_cast<int64_t>(due - now) > 0) {
                             return due;
                           }
                         }
                         removed += table_->Erase(h) ? 1 : 0;
                         return std::nullopt;
                       });
    rte_spinlock_unlock(&lock_);
    return removed;
  }

  size_t size() const noexcept { return table_->size(); }
  size_t capacity() const noexcept { return table_->capacity(); }
  const TimeoutPolicy &policy() const noexcept { return policy_; }
  Entry *Find(const CtKey &key) const noexcept { return table_->Find(key); }

 private:
  SharedConntrack(std::unique_ptr<Table> table, std::unique_ptr<Wheel> wheel,
                  const TimeoutPolicy &policy)
      : table_(std::move(table)), wheel_(std::move(wheel)), policy_(policy) {
    rte_spinlock_init(&lock_);
  }

  static void LockEntry(Entry &e) noexcept {
    while (e.lock.exchange(1, std::memory_order_acquire) != 0) {
      while (e.lock.load(std::memory_order_relaxed) != 0) {
        rte_pause();
      }
    }
  }
  static void UnlockEntry(Entry &e) noexcept { e.lock.store(0, std::memory_order_release); }

  Result Existing(const ParsedFlowPacket &p, const CanonicalKey &c, flow::FlowHandle h, Entry &e,
                  Tick now, bool may_create) noexcept {
    LockEntry(e);
    Direction dir = c.src_is_a == e.initiator_is_a ? Direction::kOriginal : Direction::kReply;
    TrackStatus status = TrackStatus::kExisting;
    switch (ct_internal::StepExisting(e, p, dir, may_create)) {
      case ct_internal::Step::kInvalid:
        UnlockEntry(e);
        return {TrackStatus::kInvalid, dir};
      case ct_internal::Step::kReopen:
        e.tcp = TcpState::kSynSent;
        e.initiator_is_a = c.src_is_a;
        e.replied = false;
        e.user = UserData{};
        dir = Direction::kOriginal;
        status = TrackStatus::kNew;
        break;
      case ct_internal::Step::kExisting:
        break;
    }
    const Tick due = Wheel::After(now, ct_internal::TimeoutFor(policy_, p.l4, e));
    e.deadline.store(due, std::memory_order_relaxed);
    UnlockEntry(e);
    std::atomic_thread_fence(std::memory_order_seq_cst);  // pairs with Expire's (Dekker)
    if (static_cast<int64_t>(e.armed.load(std::memory_order_relaxed) - due) > 0) [[unlikely]] {
      // Earlier than the armed timer (a TCP close shortens the timeout): move
      // the timer, as the owned tracker does on every packet.
      rte_spinlock_lock(&lock_);
      if (table_->Lookup(h) == &e && wheel_->Refresh(e.timer, due)) {
        e.armed.store(due, std::memory_order_relaxed);
      }
      rte_spinlock_unlock(&lock_);
    }
    return {status, dir, h, &e};
  }

  Result Create(const ParsedFlowPacket &p, const CanonicalKey &c, Tick now,
                bool may_create) noexcept {
    TcpState tcp = TcpState::kNone;
    if (p.l4 == L4Kind::kTcp) {
      const auto cls = ct_internal::ClassOf(p.tcp_flags);
      if (!ct_internal::ValidFlags(p.tcp_flags)) {
        return {TrackStatus::kInvalid};
      }
      if (cls == ct_internal::kSyn) {
        tcp = TcpState::kSynSent;
      } else if (cls == ct_internal::kAck && policy_.tcp_pickup) {
        tcp = TcpState::kEstablished;
      } else {
        return {TrackStatus::kInvalid};
      }
    } else if ((p.l4 == L4Kind::kIcmp || p.l4 == L4Kind::kIcmpv6) &&
               !ct_internal::IsEchoRequest(p.l4, p.icmp_type)) {
      return {TrackStatus::kInvalid};
    }
    if (!may_create) {
      return {TrackStatus::kInvalid};
    }
    struct {
      TcpState tcp;
      bool replied = false;
    } fresh{tcp};
    const Tick due = Wheel::After(now, ct_internal::TimeoutFor(policy_, p.l4, fresh));
    rte_spinlock_lock(&lock_);
    if (wheel_->full()) {
      rte_spinlock_unlock(&lock_);
      return {TrackStatus::kFull};
    }
    auto made = table_->Emplace(c.key, tcp, c.src_is_a, due);
    if (made.status == flow::EmplaceStatus::kExists && made.state != nullptr) {
      // Another worker created it after our lookup: a packet of it.
      rte_spinlock_unlock(&lock_);
      return Existing(p, c, made.handle, *made.state, now, may_create);
    }
    if (!made.created()) {
      rte_spinlock_unlock(&lock_);
      return {TrackStatus::kFull};
    }
    made.state->timer = wheel_->Schedule(due, made.handle);
    if (made.state->timer == dataplane::kNoExpiry) {
      (void)table_->Erase(made.handle);
      rte_spinlock_unlock(&lock_);
      return {TrackStatus::kFull};
    }
    rte_spinlock_unlock(&lock_);
    return {TrackStatus::kNew, Direction::kOriginal, made.handle, made.state};
  }

  Result Unkeyed(std::span<const uint8_t> frame, const ParsedFlowPacket &p,
                 uint16_t zone) noexcept {
    if (p.l3 != L3Kind::kNone && p.l4 != L4Kind::kNone &&
        (p.l4 == L4Kind::kIcmp || p.l4 == L4Kind::kIcmpv6) &&
        ct_internal::IsIcmpError(p.l4, p.icmp_type)) {
      CanonicalKey c;
      if (!ct_internal::QuotedKey(frame, p, zone, &c)) {
        return {TrackStatus::kInvalid};
      }
      const flow::FlowHandle h = table_->FindHandle(c.key).handle;
      Entry *e = h.id.value() != 0 ? table_->Lookup(h) : nullptr;
      if (e == nullptr) {
        return {TrackStatus::kInvalid};
      }
      LockEntry(*e);  // the initiator changes on a reopen
      const Direction quoted =
          c.src_is_a == e->initiator_is_a ? Direction::kOriginal : Direction::kReply;
      UnlockEntry(*e);
      return {TrackStatus::kRelated,
              quoted == Direction::kOriginal ? Direction::kReply : Direction::kOriginal, h, e};
    }
    return {TrackStatus::kUntracked};
  }

  std::unique_ptr<Table> table_;
  std::unique_ptr<Wheel> wheel_;
  TimeoutPolicy policy_;
  rte_spinlock_t lock_;  // creates and expiry
};

}  // namespace bess::conntrack

#endif  // BESS_CONNTRACK_SHARED_CONNTRACK_H_
