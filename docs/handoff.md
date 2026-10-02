# Handoff: moving packets to another thread and back

`dataplane/handoff.h` moves packets, each with a small typed context, from a
producer (a worker whose packets missed a policy, a reassembly stage, a flow that
needs a neighbour resolved) to a consumer that does what the packet loop must
not: a service worker, a slow path, a crypto or DPI thread, a control thread. It
does not know why a packet is handed off. No miss reason, policy or protocol is
part of it; the application defines the `Context` and what it means.
`dataplane/continuation.h` is the other half: the token a handed-off packet
carries so that whoever resumes it finds, safely, where it was going. Roadmap
milestone M11, Decision D-054 (mechanism choice, measurements, what was not
done). Experimental API: installed, may change without source compatibility.

Both headers depend on `dataplane/strong_id.h`, `generation_handle.h` and the
opaque `packet_handle.h` (an `rte_mbuf *`); no module, gate, worker, runtime,
flow table, packet view or protobuf (`tools/check_includes.py`). A graph adapter
(a `Queue`-style module) would sit in `modules/`; none exists yet.

## The shape of the API

```cpp
struct MissContext {                      // trivially copyable, one cache line at most
  ContinuationHandle continuation;        // generation-safe id for anything larger
  uint32_t reason;                        // the application's, not BESS's
};
using Punt   = HandoffChannel<MissContext, HandoffTopology::kSpSc>;
using Resume = HandoffChannel<Verdict,     HandoffTopology::kSpSc>;

// Once, at setup. The only allocation the channel ever makes.
HandoffConfig config{.capacity = 1024, .socket = numa_node_of_consumer};
auto punt = *Punt::Create(config);        // std::expected<Ptr, HandoffCreateError>

// Producer, in the packet loop: hand over a burst, keep what was refused.
PuntItem<MissContext> items[32];
size_t n = 0;
for (PacketRef pkt : misses) items[n++] = {pkt.handle(), MissContext{h, why}};
PuntResult r = punt->TryPuntBurst({items, n});
//   items[0 .. r.accepted) are the channel's now: their .packet is nullptr.
//   items[r.accepted .. n) are still yours: retry, another path, or drop.
if (r.refused) {                          // HandoffError::kFull or kClosed
  for (auto &i : std::span(items, n).subspan(r.accepted)) bess::PacketFree(i.packet);
}

// Consumer: take a burst; every packet is the consumer's from here.
PuntItem<MissContext> in[32];
size_t got = punt->Dequeue(in);
```

`TryPunt(PacketHandle &packet, const Context &)` is the one-packet form (the
roadmap's sketch): on success it sets `packet` to `nullptr`, on failure it
leaves it alone and returns `std::unexpected(HandoffError)`. Use the burst form
whenever a loop has more than one packet: a per-packet interface costs a ring
operation per packet (measured in D-054).

A reply travels the other way on a second channel (`Resume` above), with the
`ContinuationHandle` inside its context. There is no combined "pair" type: a
service thread that answers every request is one `Dequeue` and one
`TryPuntBurst`.

## Ownership

A packet belongs to exactly one side at every instant:

```text
producer --TryPuntBurst--> channel --Dequeue--> consumer
```

- **Producer to channel.** After `TryPuntBurst` returns `r`, the first
  `r.accepted` items belong to the channel, and the call has set their
  `packet` to `nullptr`: moved-from, as `std::move` of an owning pointer would
  leave it, so freeing or reusing a handed-off packet finds a null, not a
  dangling pointer. The items from `r.accepted` on were not taken and are
  untouched. The channel never frees a refused packet and keeps no reference to
  it.
- **Channel to consumer.** A packet is the consumer's from the moment `Dequeue`
  writes it into the output array, and is delivered at most once. The ring's
  slot still holds a stale copy of the pointer; nothing reads it again.
- **Nothing is in between.** A packet that is neither delivered nor refused is
  impossible: whatever is queued at teardown is freed by the channel (below).

`PacketHandle` is a raw `rte_mbuf *`; a move-only owning packet type does not
exist in this tree, so "moved-from" is enforced by the call writing `nullptr`,
and the compiler cannot stop a caller copying the pointer first. The roadmap's
sketch (`TryPunt(PacketHandle, Context)` returning `expected<void, ...>` by
value) would leave the caller holding a live-looking pointer after success.

## Back-pressure

A full channel refuses and says so: `PuntResult::refused` is `kFull`, the
refused items stay with the caller, and `refused_full` counts them. Nothing
blocks and nothing grows: the capacity is exact (the ring is sized to the next
power of two, but refuses at `capacity`) and fixed at `Create`. What to do with
a refused tail is the producer's policy, and there are two:

- **fail/drop**: free the tail and count it (the caller's counter or
  `refused_full`);
- **retry or alternate path**: offer it again later or send it somewhere else.

There is no third, hidden policy. Do not spin on a full channel in a packet loop:
a consumer that has stopped makes the spin permanent. Close it instead.

## Topology

`HandoffTopology` is a template argument, so the enqueue and dequeue are the
ring's explicit single- or multi-producer entry points and nothing inspects the
topology per call:

| Topology | Producers | Consumers | Enqueue | Dequeue |
|---|---|---|---|---|
| `kSpSc` | one thread | one thread | single-producer | single-consumer |
| `kMpSc` | many | one | compare-and-swap on the head | single-consumer |
| `kMpMc` | many | many | compare-and-swap | compare-and-swap |

"One thread" means one at a time: a role may move between threads if the move
synchronises (a join, a lock). Two threads in an `kSpSc` role corrupt the ring
silently; nothing detects it. A `Queue`-style adapter that picks between SP and
MP from the number of upstream workers (D-036) holds one channel of each type
and chooses at `PreResume`, not per burst.

## Lifecycle: the order that makes teardown safe

1. `Close()`: from now on `TryPuntBurst` refuses everything with `kClosed`, so
   producers fall back at once. Idempotent, any thread. Packets already queued
   stay queued.
2. Stop every producer and consumer thread: a quiescent point (workers paused,
   joined, or past an RCU grace period). The channel cannot wait for threads
   inside a call; destroying it under one is a use after free, as for any object.
3. `Drain(fn)`, optional: takes what is left one item at a time and hands each to
   `fn(PuntItem &)`, which owns its packet from then on (forward it to the slow
   path's own queue, count it, free it). Counted in `discarded`.
4. Destroy the channel. Whatever is still queued is freed with
   `rte_pktmbuf_free` and counted in `discarded`.

A consumer that crashes, or never drains, is steps 1-4 run by the owner. Until
then its producers see `kFull` and drop, so memory stays bounded. A consumer that
has already dequeued a burst owns it and must finish with it; a crash of the
whole process is not handled here.

When no call is in progress: `enqueued == dequeued + discarded + occupancy`, and
every item a producer offered was enqueued, refused (full or closed), or is
still the producer's. `stats()` returns the counters; a snapshot taken while
threads run can show a dequeue before its enqueue.

## Context

`Context` must be trivially copyable and the whole item (8-byte packet pointer
plus context) at most 64 bytes, checked at compile time. An empty context
(`NoContext`, the default) makes the slot the pointer alone: 8 bytes. The context
is copied through the ring in the same slot as the packet; carrying it in the
packet's private area instead was measured to cost more (D-054). Anything larger
or not trivially copyable is carried as a generation-safe id (a `FlowHandle`, a
`ContinuationHandle`) into a table the application owns.

Element sizes of 8 and 16 bytes use DPDK's dedicated copy paths; other sizes
(24, 32, 64) copy 32-bit words and cost slightly more per item. Prefer 8, 16 or
32.

## Continuations: resuming safely

A packet parked in a queue or a service thread outlives the structure it was
headed for. `ContinuationHandle` is a `ContinuationId` plus the generation of its
slot when it was issued (8 bytes, one 64-bit compare), and
`ContinuationTable<Target>` is the store behind it:

```cpp
auto table = *ContinuationTable<Hop>::Create(4096);   // one allocation
ContinuationHandle h = table->Issue(Hop{iface, stage}); // kNoContinuation if full
// ... h travels in the punt's context, comes back in the reply's ...
if (std::optional<Hop> hop = table->Resolve(h)) resume_at(*hop);
else                                             drop_stale(packet);
table->Retire(h);                               // the target went away
```

- `Target` is any trivially copyable value up to 64 bytes: the application's
  meaning (an `InterfaceId` and a stage number, an index into its own
  RCU-protected table). The table copies it in and out; it never holds a pointer
  to anything the application owns, and a continuation is never a raw graph
  pointer or a reusable integer.
- `Retire` kills every copy of the handle at once. A stale, forged or retired
  handle resolves to nothing and retires nothing, even after its slot is reused
  (`StaleHandleCannotReachAContinuationThatReusedItsSlot`, the test
  architecture.md section 7 asks of every handle-issuing battery).
- Slots are reused oldest-freed first, so a handle parked in a queue sees the
  longest possible time before its slot can hold anything else; a slot whose
  32-bit generation would wrap is retired for good (`quarantined()`).
- One thread at a time may `Issue` and `Retire` (typically the worker that
  punts); any number of threads may `Resolve` concurrently, lock-free, with no
  read-modify-write. A service thread can therefore check a continuation before
  it spends effort on a packet whose target is gone, without being an RCU reader.
- The validity of a continuation does not depend on RCU quiescence of any CPU:
  an accelerator or a hardware completion is checked by generation alone.

## Memory and NUMA

One allocation in `Create` from `HandoffConfig::allocator` (DPDK's heap on the
requested node by default): the channel object, then the ring. After it nothing
allocates. `memory_bytes()` is the whole block; at full occupancy the cost per
item is `memory_bytes() / capacity()`.

`placement()` reports the node asked for, the node the memory landed on (the
default allocator falls back to any node when the one asked for is out of memory)
and `misplaced()`. Place the channel where its busier side runs. The producer and
consumer threads are the application's, so a channel whose ends sit on different
nodes pays the interconnect on every item; compare `placement()` with where the
threads run. Cross-NUMA handoff has not been measured: the development machine
has one node (D-054).

## Failure injection and tests

`core/dataplane/handoff_test.cc` (single thread: ownership, back-pressure,
exhaustive index wrap-around, a random differential test against a queue,
allocator refusal, placement, packet accounting against the pool),
`handoff_threads_test.cc` (real threads: SP/SC, MP/SC and MP/MC streams, a
consumer that never drains, shutdown in order, a consumer that stops mid-stream,
repeated lifecycle, punt-service-resume with retired continuations) and
`continuation_test.cc`. None asserts how much work fits in a time window. They run
pinned to one or two CPUs; the cross-thread ones have a ThreadSanitizer run
described in D-054.

Benchmarks: `handoff_bench` (build and run from the release tree, pinned to two
physical cores; see benchmarking.md).
