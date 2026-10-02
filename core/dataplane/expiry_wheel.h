// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_EXPIRY_WHEEL_H_
#define BESS_DATAPLANE_EXPIRY_WHEEL_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <glog/logging.h>

#include "dataplane/generation_handle.h"
#include "dataplane/strong_id.h"

// A worker-owned expiry engine (roadmap M10, Decision D-053; guide in
// docs/expiry.md). Experimental API.
//
// What it is. A set of timers, each holding one `Payload` (a handle or an
// index: whatever lets the owner find the thing that is expiring) and a
// deadline in ticks. The owner arms, moves and cancels timers, and calls
// Poll(now, budget) between batches; Poll hands each due timer's payload to a
// callback, and the owner does the semantic work (erase the flow, drop the
// entry). The engine never touches the owner's objects, so it has no
// dependency on flow tables, modules, gates or workers, and a flow table, an
// FDB, a neighbour cache and a decision cache can all use it.
//
// Mechanism. A hierarchical timing wheel (`Levels` rings of 2^LevelBits
// slots, each level 2^LevelBits times coarser than the one below) over a
// fixed array of 32-byte nodes (24 with 32-bit ticks) linked by 32-bit
// indices. Why this and not rte_timer, a heap, a hashed wheel or a periodic
// scan is measured in D-053.
//   Schedule/Cancel  O(1): at most a few stores, no search, no allocation.
//   Refresh to a later deadline (the idle-timeout hot path)
//                    one load, compare and store on the node: the node is not
//                    moved. When the wheel reaches its old position it moves
//                    it once, so a flow refreshed on every packet is moved at
//                    most once per timeout, not once per packet.
//   Poll             does at most `budget` units of work, however many timers
//                    are due (below), and skips empty stretches of time using
//                    one occupancy word per level.
//
// Time. A tick is whatever the caller's counter counts; the engine is told no
// rate (see tick_rate.h for the control-boundary conversion). Ticks are an
// unsigned integer type that may wrap: all comparisons are serial-number
// arithmetic, so the only requirements are
//   - Poll(now) is called with a non-decreasing `now` (an earlier `now` than
//     a previous call is treated as equal to it), and at least once per
//     kMaxTimeout ticks;
//   - a deadline is at most kMaxTimeout ticks after the last polled time.
//     Longer timeouts must be clamped with After() (a "forever" timeout is
//     After(now, kMaxTimeout)); an unclamped one wraps into the past and
//     fires at once.
// A timer whose deadline lies beyond the wheel's span (2^(g + LevelBits *
// Levels) ticks) is not wrong, only costlier: it is re-placed once per
// revolution of the top level. Choose g so the span covers the ordinary
// timeouts (for 64-bit ticks g can be up to 27, a span of 2^63).
// `granularity_shift` g makes the wheel's own tick 2^g counter ticks: a timer
// fires on the first Poll whose `now`, rounded down to a multiple of 2^g, is
// at least the deadline rounded up to one. It never fires early and is late
// by less than 2^g ticks (zero late for g = 0). A deadline at or before the
// time of the last Poll fires on the next Poll that has budget.
//
// Handles. Schedule returns an ExpiryHandle (node index + generation). It
// names one arming: after the timer fires or is cancelled the handle is dead
// for good, even once the node holds another timer, and Refresh/Cancel on it
// return false without touching the new timer. The payload carries the
// owner's own generation check (a FlowHandle, for a flow table), so a record
// that outlives its target does nothing harmful in two independent ways.
//
// Budget. Poll(now, budget, fn) delivers or moves at most `budget` timers in
// all: each due timer costs one unit, and so does each timer moved down a
// level or re-armed because it was refreshed. A timer is moved at most
// Levels - 1 times before it fires, so a storm of any size finishes in a
// bounded number of polls. A Poll that stops early reports `exhausted` and
// the next call resumes exactly where it stopped; nothing is lost or
// delivered twice.
//
// Order. Timers fire in non-decreasing deadline order at the wheel's
// granularity. Timers with the same deadline, armed at the same wheel time,
// fire in the order they were armed. The order of equal deadlines armed at
// different times is deterministic (a function of the operation history only)
// but is not the arming order. A timer already late when armed fires before
// any other timer.
//
// Ownership. One thread only: nothing is atomic or locked. Typically a
// worker owns its engine and polls it from its own task. Memory is allocated
// once in Create(); afterwards no operation allocates, and a full engine says
// so (Schedule returns kNoExpiry) and changes nothing.
//
// Callbacks. The callback must be noexcept. It may call Schedule, Cancel
// (including of the timer being delivered, which is how a flow table's
// OnErase cancels a flow's own timer when the expiry erases the flow) and
// Refresh of other timers; Refresh of the timer being delivered returns false
// and it must not call Poll. It returns void (the timer is spent) or
// std::optional<Tick>: nullopt (spent) or a new deadline for the same timer,
// same handle, for owners that keep the real deadline in their own state and
// refresh it with a plain store (see docs/expiry.md).

namespace bess::dataplane {

struct ExpiryIdTag;
using ExpiryId = StrongId<ExpiryIdTag, uint32_t>;
// Names one arming of a timer; see "Handles" above.
using ExpiryHandle = GenerationHandle<ExpiryId>;
// The handle of no timer; also what Schedule returns for a full engine.
inline constexpr ExpiryHandle kNoExpiry{};

enum class ExpiryError : uint8_t {
  kInvalidCapacity,     // zero, or more timers than 32-bit indices name
  kInvalidGranularity,  // granularity shift too large for the tick type
  kOutOfMemory,         // the allocator refused
};

inline const char *ToString(ExpiryError error) noexcept {
  switch (error) {
    case ExpiryError::kInvalidCapacity:
      return "invalid expiry engine capacity";
    case ExpiryError::kInvalidGranularity:
      return "expiry granularity does not fit the tick type";
    case ExpiryError::kOutOfMemory:
      return "out of memory creating the expiry engine";
  }
  return "unknown expiry engine error";
}

// What one Poll did.
struct PollResult {
  size_t fired = 0;  // callbacks made (a re-armed timer counts each time)
  size_t moved = 0;  // timers moved down a level or re-placed after a refresh
  bool exhausted = false;  // stopped on the budget with due work left

  // The units charged against the budget; never more than the budget.
  size_t work() const noexcept { return fired + moved; }
};

template <typename Payload, std::unsigned_integral Tick = uint64_t,
          unsigned LevelBits = 6, unsigned Levels = 6>
  requires std::is_trivially_copyable_v<Payload> && (LevelBits >= 1) &&
           (LevelBits <= 6) && (Levels >= 1) &&
           (LevelBits * Levels < std::numeric_limits<Tick>::digits)
class ExpiryWheel {
 public:
  using payload_type = Payload;
  using tick_type = Tick;

  // The longest timeout the engine can order correctly, and the longest the
  // gap between two Polls may be: a quarter of the tick range, so that a
  // deadline set after a long gap, plus the rounding to the granularity, still
  // stays under half the range, where serial-number order is unambiguous.
  static constexpr Tick kMaxTimeout = std::numeric_limits<Tick>::max() >> 2;
  // The largest granularity shift for this tick type.
  static constexpr unsigned kMaxGranularityShift =
      std::numeric_limits<Tick>::digits - 1 - LevelBits * Levels;

  // `now + timeout`, with the timeout clamped to kMaxTimeout so a very long
  // one cannot wrap into the past.
  static constexpr Tick After(Tick now, Tick timeout) noexcept {
    return static_cast<Tick>(now + (timeout > kMaxTimeout ? kMaxTimeout : timeout));
  }

  // An empty engine for up to `capacity` timers, starting at time `start`.
  // The only allocation it ever makes happens here.
  static std::expected<std::unique_ptr<ExpiryWheel>, ExpiryError> Create(
      size_t capacity, Tick start = 0, unsigned granularity_shift = 0) {
    if (capacity == 0 ||
        capacity > std::numeric_limits<uint32_t>::max() - kFirst) {
      return std::unexpected(ExpiryError::kInvalidCapacity);
    }
    if (granularity_shift > kMaxGranularityShift) {
      return std::unexpected(ExpiryError::kInvalidGranularity);
    }
    const size_t count = kFirst + capacity;
    void *memory = ::operator new(count * sizeof(Node), std::align_val_t(64),
                                  std::nothrow);
    if (memory == nullptr) {
      return std::unexpected(ExpiryError::kOutOfMemory);
    }
    Nodes nodes(static_cast<Node *>(memory));
    std::unique_ptr<ExpiryWheel> wheel(
        new (std::nothrow) ExpiryWheel(std::move(nodes), capacity,
                                       start, granularity_shift));
    if (wheel == nullptr) {
      return std::unexpected(ExpiryError::kOutOfMemory);
    }
    return wheel;
  }

  ExpiryWheel(const ExpiryWheel &) = delete;
  ExpiryWheel &operator=(const ExpiryWheel &) = delete;

  // -- arming -------------------------------------------------------------------

  // Arms a timer that delivers `payload` at `deadline`. Returns kNoExpiry,
  // changing nothing, if the engine holds `capacity` timers.
  ExpiryHandle Schedule(Tick deadline, const Payload &payload) noexcept {
    if (free_head_ == kNil) {
      return kNoExpiry;
    }
    const uint32_t index = free_head_;
    Node &node = nodes_[index];
    free_head_ = node.next;
    node.generation++;  // even -> odd: armed
    node.deadline = deadline;
    node.payload = payload;
    size_++;
    Place(index);
    return {ExpiryId(index), node.generation};
  }

  // Moves the timer to `deadline`. A later deadline than the timer's present
  // one is a store into the node and nothing else; an earlier one moves the
  // node. False -- and nothing happens -- if the handle is stale or the timer
  // is the one being delivered by the running Poll.
  bool Refresh(ExpiryHandle handle, Tick deadline) noexcept {
    Node *node = Resolve(handle);
    if (node == nullptr || Delivering(handle.id.value())) {
      return false;
    }
    const bool later = Diff(deadline, node->deadline) >= 0;
    node->deadline = deadline;
    if (!later) {
      const uint32_t index = handle.id.value();
      Unlink(index);
      Place(index);
    }
    return true;
  }

  // Disarms the timer. False if the handle is stale (it already fired, was
  // cancelled, or never existed). Safe from inside a Poll callback.
  bool Cancel(ExpiryHandle handle) noexcept {
    Node *node = Resolve(handle);
    if (node == nullptr) {
      return false;
    }
    const uint32_t index = handle.id.value();
    Unlink(index);
    Free(index);
    return true;
  }

  // -- polling ------------------------------------------------------------------

  // Delivers the payloads of timers due at `now` to `fn`, doing at most
  // `budget` units of work (see "Budget"). `fn(const Payload &)` returns void
  // or std::optional<Tick>.
  template <typename Fn>
  PollResult Poll(Tick now, size_t budget, Fn &&fn) noexcept {
    static_assert(std::is_nothrow_invocable_v<Fn &, const Payload &>,
                  "an expiry callback must be noexcept");
    DCHECK(!polling_) << "Poll is not re-entrant";
    polling_ = true;
    const PollResult result = PollImpl(now, budget, fn);
    polling_ = false;
    return result;
  }

  // -- state --------------------------------------------------------------------

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  bool full() const noexcept { return free_head_ == kNil; }
  // Nodes retired because their generation was exhausted (2^31 armings of one
  // node); they lower the usable capacity.
  size_t quarantined() const noexcept { return quarantined_; }
  // The engine's own time: the last polled time, rounded down to the
  // granularity.
  Tick wheel_time() const noexcept { return cur_; }
  unsigned granularity_shift() const noexcept { return shift_; }

  bool Alive(ExpiryHandle handle) const noexcept {
    return Resolve(handle) != nullptr;
  }
  // The deadline the timer is armed for, or nullopt for a stale handle.
  std::optional<Tick> DeadlineOf(ExpiryHandle handle) const noexcept {
    const Node *node = Resolve(handle);
    return node == nullptr ? std::nullopt : std::optional<Tick>(node->deadline);
  }

  // Bytes of engine memory; the per-timer cost at full occupancy is
  // memory_bytes() / capacity().
  size_t memory_bytes() const noexcept {
    return (kFirst + capacity_) * sizeof(Node) + sizeof(*this);
  }
  static constexpr size_t node_bytes() noexcept { return sizeof(Node); }

  // Testing only: sets the generation of the node the next Schedule will use
  // (even), to reach the wrap-around without 2^31 armings.
  void SetNextNodeGenerationForTesting(uint32_t generation) noexcept {
    CHECK_NE(free_head_, kNil);
    CHECK_EQ(0u, generation & 1);
    nodes_[free_head_].generation = generation;
  }

 private:
  using Signed = std::make_signed_t<Tick>;

  static constexpr unsigned kSlotBits = LevelBits;
  static constexpr uint32_t kSlots = uint32_t{1} << kSlotBits;
  static constexpr uint32_t kSlotMask = kSlots - 1;
  static constexpr uint32_t kWheelNodes = Levels * kSlots;  // slot list heads
  static constexpr uint32_t kReady = kWheelNodes;           // due, in order
  static constexpr uint32_t kMoving = kWheelNodes + 1;      // to be re-placed
  static constexpr uint32_t kFirst = kWheelNodes + 2;       // first real timer
  static constexpr uint32_t kNil = 0;  // free list end; a list head, never a timer

  // Timers and list heads share one array: a head is a node whose payload and
  // deadline are unused, so a node's neighbours are always valid nodes and
  // unlinking needs no case for "first" or "last". Lists are circular.
  //
  // A node is in exactly one of: the free list (even generation, `next`
  // links), a wheel slot, kReady or kMoving (odd generation), or being
  // delivered (odd generation, linked to itself).
  struct Node {
    uint32_t prev;
    uint32_t next;
    uint32_t generation;
    Tick deadline;
    Payload payload;
  };

  struct Deleter {
    void operator()(Node *p) const noexcept {
      ::operator delete(p, std::align_val_t(64));
    }
  };
  using Nodes = std::unique_ptr<Node[], Deleter>;

  ExpiryWheel(Nodes nodes, size_t capacity, Tick start, unsigned shift)
      : nodes_(std::move(nodes)),
        capacity_(static_cast<uint32_t>(capacity)),
        shift_(shift),
        cur_(static_cast<Tick>(start & ~UnitMask(shift))) {
    for (uint32_t head = 0; head < kFirst; head++) {
      nodes_[head] = Node{};
      nodes_[head].prev = nodes_[head].next = head;
    }
    for (uint32_t i = 0; i < capacity_; i++) {
      Node &node = nodes_[kFirst + i];
      node = Node{};
      node.next = (i + 1 == capacity_) ? kNil : kFirst + i + 1;
    }
    free_head_ = kFirst;
  }

  static constexpr Tick UnitMask(unsigned shift) noexcept {
    return static_cast<Tick>((Tick{1} << shift) - 1);
  }
  Tick Unit() const noexcept { return static_cast<Tick>(Tick{1} << shift_); }

  // Serial-number difference a - b: positive if a is after b.
  static Signed Diff(Tick a, Tick b) noexcept {
    return static_cast<Signed>(static_cast<Tick>(a - b));
  }

  // The timer for a live, not-yet-fired handle, or nullptr.
  Node *Resolve(ExpiryHandle handle) noexcept {
    const uint32_t index = handle.id.value();
    if (index < kFirst || index - kFirst >= capacity_) {
      return nullptr;
    }
    Node &node = nodes_[index];
    return (node.generation == handle.generation && (node.generation & 1))
               ? &node
               : nullptr;
  }
  const Node *Resolve(ExpiryHandle handle) const noexcept {
    return const_cast<ExpiryWheel *>(this)->Resolve(handle);
  }

  bool Delivering(uint32_t index) const noexcept {
    return nodes_[index].prev == index;
  }

  // The deadline rounded up to a multiple of the wheel's tick.
  Tick EndOf(Tick deadline) const noexcept {
    const Tick low = static_cast<Tick>(deadline & UnitMask(shift_));
    return static_cast<Tick>(deadline - low + (low != 0 ? Unit() : Tick{0}));
  }

  // -- lists ---------------------------------------------------------------------

  void Link(uint32_t head, uint32_t index) noexcept {
    Node &list = nodes_[head];
    const uint32_t last = list.prev;
    nodes_[index].prev = last;
    nodes_[index].next = head;
    nodes_[last].next = index;
    list.prev = index;
    if (head < kWheelNodes) {
      occupied_[head >> kSlotBits] |= uint64_t{1} << (head & kSlotMask);
    }
  }

  // Removes the node from whatever list holds it. A slot list that becomes
  // empty clears its occupancy bit.
  void Unlink(uint32_t index) noexcept {
    Node &node = nodes_[index];
    const uint32_t prev = node.prev, next = node.next;
    nodes_[prev].next = next;
    nodes_[next].prev = prev;
    if (prev == next && prev < kWheelNodes) {
      occupied_[prev >> kSlotBits] &= ~(uint64_t{1} << (prev & kSlotMask));
    }
  }

  // Appends the whole of `from` to `to` (both list heads).
  void Splice(uint32_t from, uint32_t to) noexcept {
    Node &src = nodes_[from];
    if (src.next == from) {
      return;
    }
    Node &dst = nodes_[to];
    const uint32_t first = src.next, last = src.prev, tail = dst.prev;
    nodes_[tail].next = first;
    nodes_[first].prev = tail;
    nodes_[last].next = to;
    dst.prev = last;
    src.next = src.prev = from;
    if (from < kWheelNodes) {
      occupied_[from >> kSlotBits] &= ~(uint64_t{1} << (from & kSlotMask));
    }
  }

  void Free(uint32_t index) noexcept {
    Node &node = nodes_[index];
    size_--;
    node.generation++;  // odd -> even: every handle to this arming is dead
    if (node.generation == 0) [[unlikely]] {
      // Wrapped: this node's first arming's handles would match its next.
      // Retire it rather than reuse it.
      quarantined_++;
      return;
    }
    node.next = free_head_;
    free_head_ = index;
  }

  // -- placement -----------------------------------------------------------------

  // Links the (unlinked, armed) node where it will next be looked at: the due
  // list if its deadline has been reached, else the slot of the lowest level
  // that can hold it. Deadlines beyond the whole wheel go to the farthest slot
  // of the top level and are re-placed when the wheel reaches it.
  void Place(uint32_t index) noexcept {
    const Tick end = EndOf(nodes_[index].deadline);
    if (Diff(end, cur_) <= 0) {
      Link(kReady, index);
      return;
    }
    for (unsigned level = 0; level < Levels; level++) {
      const unsigned sh = shift_ + level * kSlotBits;
      const Tick span_mask = static_cast<Tick>(std::numeric_limits<Tick>::max() >> sh);
      const Tick ahead =
          static_cast<Tick>(((end >> sh) - (cur_ >> sh)) & span_mask);
      if (ahead <= kSlotMask) {
        DCHECK_GE(ahead, 1u);
        Link(level * kSlots + (static_cast<uint32_t>(end >> sh) & kSlotMask),
             index);
        return;
      }
    }
    const unsigned sh = shift_ + (Levels - 1) * kSlotBits;
    const uint32_t slot =
        (static_cast<uint32_t>(cur_ >> sh) + kSlotMask) & kSlotMask;
    Link((Levels - 1) * kSlots + slot, index);
  }

  // -- time ----------------------------------------------------------------------

  // The distance from cur_ to the next time at which something must be looked
  // at (a non-empty level-0 slot, or a boundary where a non-empty higher slot
  // is moved down), or nothing if the wheel is empty.
  bool NextEvent(Tick *distance) const noexcept {
    bool found = false;
    Tick best = 0;
    for (unsigned level = 0; level < Levels; level++) {
      const uint64_t bits = occupied_[level];
      if (bits == 0) {
        continue;
      }
      const unsigned sh = shift_ + level * kSlotBits;
      const uint32_t here = static_cast<uint32_t>(cur_ >> sh) & kSlotMask;
      // Rotate so that bit 0 is the slot after this one, then take the first.
      const uint32_t start = (here + 1) & kSlotMask;
      const uint64_t rotated =
          start == 0 ? bits
                     : ((bits >> start) | (bits << (kSlots - start))) &
                           SlotsMask();
      const unsigned d = static_cast<unsigned>(std::countr_zero(rotated)) + 1;
      const Tick boundary = static_cast<Tick>(((cur_ >> sh) + d) << sh);
      const Tick dist = static_cast<Tick>(boundary - cur_);
      if (!found || dist < best) {
        best = dist;
        found = true;
      }
    }
    *distance = best;
    return found;
  }

  static constexpr uint64_t SlotsMask() noexcept {
    return kSlots == 64 ? ~uint64_t{0} : (uint64_t{1} << kSlots) - 1;
  }

  // Moves the wheel to `t` (a boundary at which something is due) and queues
  // that time's work: this tick's slot for firing, and the slot of every
  // higher level whose range starts at `t` for re-placement.
  void Arrive(Tick t) noexcept {
    cur_ = t;
    for (unsigned level = Levels; level-- > 1;) {
      const unsigned sh = shift_ + level * kSlotBits;
      if ((t & static_cast<Tick>((Tick{1} << sh) - 1)) == 0) {
        Splice(level * kSlots + (static_cast<uint32_t>(t >> sh) & kSlotMask),
               kMoving);
      }
    }
    Splice(static_cast<uint32_t>(t >> shift_) & kSlotMask, kReady);
  }

  template <typename Fn>
  PollResult PollImpl(Tick now, size_t budget, Fn &fn) noexcept {
    Tick target = static_cast<Tick>(now & ~UnitMask(shift_));
    if (Diff(target, cur_) < 0) {
      target = cur_;
    }
    PollResult result;
    if (budget == 0) {
      Tick dist;
      result.exhausted = nodes_[kReady].next != kReady ||
                         nodes_[kMoving].next != kMoving ||
                         (NextEvent(&dist) &&
                          dist <= static_cast<Tick>(target - cur_));
      return result;
    }
    size_t work = 0;
    for (;;) {
      if (nodes_[kReady].next != kReady) {
        if (work == budget) {
          result.exhausted = true;
          return result;
        }
        work++;
        Deliver(nodes_[kReady].next, fn, &result);
        continue;
      }
      if (nodes_[kMoving].next != kMoving) {
        if (work == budget) {
          result.exhausted = true;
          return result;
        }
        work++;
        const uint32_t index = nodes_[kMoving].next;
        Unlink(index);
        Place(index);
        result.moved++;
        continue;
      }
      Tick dist;
      if (NextEvent(&dist) && dist <= static_cast<Tick>(target - cur_)) {
        Arrive(static_cast<Tick>(cur_ + dist));
        continue;
      }
      cur_ = target;
      return result;
    }
  }

  // Takes the node at the head of the due list: delivers it if it is due,
  // else (a later refresh moved its deadline past its position) puts it back
  // where the new deadline belongs.
  template <typename Fn>
  void Deliver(uint32_t index, Fn &fn, PollResult *result) noexcept {
    Unlink(index);
    Node &node = nodes_[index];
    if (Diff(EndOf(node.deadline), cur_) > 0) {
      Place(index);
      result->moved++;
      return;
    }
    const uint32_t generation = node.generation;
    const Payload payload = node.payload;
    node.prev = node.next = index;  // delivering
    result->fired++;
    if constexpr (std::is_void_v<std::invoke_result_t<Fn &, const Payload &>>) {
      fn(payload);
      if (nodes_[index].generation == generation) {
        Free(index);
      }
    } else {
      const std::optional<Tick> again = fn(payload);
      if (nodes_[index].generation == generation) {
        if (again.has_value()) {
          nodes_[index].deadline = *again;
          Place(index);
        } else {
          Free(index);
        }
      }
    }
  }

  Nodes nodes_;
  uint32_t capacity_;
  unsigned shift_;
  Tick cur_;
  uint32_t free_head_ = kNil;
  uint32_t size_ = 0;
  uint32_t quarantined_ = 0;
  bool polling_ = false;
  uint64_t occupied_[Levels] = {};
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_EXPIRY_WHEEL_H_
