# Expiry and timed state

`dataplane/expiry_wheel.h` is a worker-owned timer set for state with a
lifetime: flows, FDB entries, NAT bindings, neighbour entries, cached
decisions. It does not know what it expires. A timer holds a payload (a
`FlowHandle`, an index, anything trivially copyable) and a deadline in ticks;
`Poll` hands the payload of each due timer to a callback, and the owner does the
semantic work. Roadmap milestone M10, Decision D-053 (mechanism choice,
measurements and what was not done). Experimental API: installed, may change
without source compatibility.

The library depends on nothing but `dataplane/strong_id.h` and
`dataplane/generation_handle.h`: no module, gate, worker, runtime, flow table or
protobuf. `flow/` uses it through its `Observer` seam; the edge never runs the
other way (`tools/check_includes.py`).

## The shape of the API

```cpp
using Wheel = bess::dataplane::ExpiryWheel<flow::FlowHandle>;   // Tick = uint64_t

// Once, at setup: capacity (the only allocation), the clock's current value,
// and the wheel's granularity as a power of two of ticks.
bess::dataplane::TickRate rate(tsc_hz);                 // seconds <-> ticks
auto wheel = *Wheel::Create(max_flows, now_tsc, rate.ShiftFor(1ms));

ExpiryHandle h = wheel->Schedule(now + rate.Ticks(30s), flow_handle);
wheel->Refresh(h, now + rate.Ticks(30s));               // moves the deadline
wheel->Cancel(h);

// Between batches, with the scheduler's cached time (no clock read) and a budget:
auto r = wheel->Poll(ctx->current_tsc, /*budget=*/64,
                     [&](const flow::FlowHandle &f) noexcept { table.Erase(f); });
// r.fired callbacks made; r.exhausted: stopped on the budget with work left.
```

`Schedule` returns `kNoExpiry` when all `capacity` timers are in use and changes
nothing. An `ExpiryHandle` (index and generation) names one arming; after the
timer fires or is cancelled the handle stays dead even once the node is reused.
The deviation from the roadmap's sketch (`Refresh(ExpiryHandle &, ...)`) is that
the handle never changes, so it is passed by value.

## Time

A tick is whatever the caller's counter counts; the engine is told no rate.
`TickRate` is the single place seconds meet ticks (control boundary): it rounds
down, saturates instead of wrapping, is monotonic, and uses 128-bit products.
Hot code passes ticks only: the scheduler's cached TSC is a free `now`, and the
library never reads a clock or `tsc_hz`, so it is architecture neutral.

Ticks are an unsigned integer type (`ExpiryWheel<Payload, Tick, LevelBits,
Levels>`; `uint64_t` by default, narrower types for tests and compact state).
Comparisons are serial-number arithmetic, so a wrapping counter is fine under two
rules, both stated in the header:

- `Poll(now)` is called with a non-decreasing `now` (an earlier value is treated
  as no time passing) and at least once per `kMaxTimeout` ticks (a quarter of the
  tick range; for 64 bits that is centuries);
- a deadline is at most `kMaxTimeout` ticks after the last polled time. A longer
  or "forever" timeout is clamped with `After(now, timeout)`; an unclamped one
  would wrap into the past and fire at once.

`granularity_shift` g makes the wheel's own tick 2^g counter ticks. A timer fires
on the first `Poll` whose `now`, rounded down to a multiple of 2^g, is at least
the deadline rounded up to one: never early, late by less than 2^g (not late at
all for g = 0). For a TSC, `rate.ShiftFor(1ms)` is about 20. Choose g so that the
wheel's span, 2^(g + LevelBits * Levels) ticks (2^56 at g = 20 with the default 6
levels of 64 slots), covers the ordinary timeouts: a deadline beyond it is not
wrong, only re-placed once per revolution of the top level. A deadline at or
before the last polled time (a zero timeout) fires on the next `Poll` that has
budget.

## Poll and its budget

`Poll(now, budget, fn)` charges one unit for each due timer it delivers and one
for each timer it moves (down a level, or re-placed because a refresh pushed its
deadline past its position), and stops when `budget` is used up, however many
timers are due. `PollResult::work()` is the units used and never exceeds the
budget; `exhausted` is true if due work remains, and the next call resumes where
this one stopped without loss or duplication. A timer is moved at most
`Levels - 1` times before it fires, plus once per time a lazy refresh moved its
deadline past its position, so a storm of any size finishes in a bounded number
of polls. Empty stretches of time are skipped with one occupancy word per level,
so a poll after a long idle gap costs O(levels) per non-empty slot, not per tick.
`Poll(now, 0, fn)` does nothing and says whether work is waiting.

The callback must be `noexcept`. It returns `void` (the timer is spent) or
`std::optional<Tick>` (see below). Inside it you may `Schedule`, `Cancel` (also
the timer being delivered, which is what a flow table's `OnErase` does when the
expiry erases the flow) and `Refresh` other timers; `Refresh` of the delivered
timer returns false, and `Poll` must not be called.

## Refreshing a hot flow

Two ways, both tested against a model, with different costs (D-053):

1. **`Refresh(handle, now + timeout)`.** A later deadline is a load, compare and
   store on the engine's node: no unlink, no list traffic. The wheel notices only
   when it reaches the node's old position, and moves it once. A flow touched on
   every packet is moved once per timeout, not once per packet. An earlier
   deadline relinks the node.
2. **Owner-side refresh.** The State keeps `last_seen`; a hit stores `now` into
   it (the line the lookup just read) and the engine is not called at all. The
   engine's record is then only a lower bound; when it fires early the callback
   looks at the State and returns the real deadline (`return last_seen +
   timeout;`) to re-arm the same timer (same handle), or `std::nullopt` to
   expire. Use this when the per-packet cost of touching a second array matters.

## Flow-table wiring

`core/flow/expiry_consumer_test.cc` is the reference consumer, written against
public headers only. In outline:

```cpp
struct Conn { uint64_t last_seen; ExpiryHandle timer; /* ... */ };
struct IdleObserver {                       // WorkerFlowTable's Traits::Observer
  Wheel *wheel; const uint64_t *now; uint64_t timeout;
  void OnCreate(FlowHandle h, Conn &c) noexcept { c.timer = wheel->Schedule(*now + timeout, h); }
  void OnErase(FlowHandle, Conn &c) noexcept { wheel->Cancel(c.timer); }
  void OnFull() noexcept {}
};
// A packet: Find, then c->last_seen = now (owner side) or wheel.Refresh(c->timer, ...).
// Between batches: wheel.Poll(now, 64, [&](FlowHandle h) noexcept { table.Erase(h); });
```

Give the wheel at least as many timers as the table has flows (twice as many if
`OnErase` does not cancel). If the owner forgets to cancel, a record outlives its
flow and fires with a stale `FlowHandle`; `table.Erase(handle)` then returns
false and the flow that reused the slot is untouched. That is the generation
check of M9, and the engine does not weaken it: it hands back exactly the payload
it was given.

## Order

Timers fire in non-decreasing deadline order at the wheel's granularity. Timers
with one deadline, armed at the same wheel time, fire in the order they were
armed. Equal deadlines armed at different times fire in an order that depends
only on the operation history (nothing about addresses, hashes or time), but not
necessarily the arming order. A timer that is already late when armed is
delivered before all others.

## Ownership and memory

One thread: nothing is atomic. A worker owns its wheel and polls it from its own
task; nothing here is callable from another thread (there is no owner-check
policy as in `flow/owner.h`). `Create` allocates `capacity` nodes (32 bytes each
with 64-bit ticks and an 8-byte payload, 24 with 32-bit ticks, plus 386 list
heads for the default shape) aligned to cache lines, and nothing allocates
afterwards. The owner stores the 8-byte `ExpiryHandle` per timer (typically in
the flow's State). A node whose generation would wrap (2^31 armings of one node)
is retired instead of reused, like a flow slot (`quarantined()`).

## When not to use it

For a dense table whose entries all share one coarse timeout and whose churn is
high, a periodic scan of the table (`WorkerFlowTable::ForEach`, or a budgeted
cursor) can cost less than a wheel; D-053 gives the break-even from measurements.
The wheel's cost does not grow with the table or the number of idle entries, its
detection delay does not grow with the table, and its work per poll is bounded;
a scan has none of those properties unless it is itself budgeted.
