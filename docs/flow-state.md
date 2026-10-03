# Flow state: typed tables with stable ids

`core/flow/` (library `bess_flow`, roadmap M9, [D-052](decisions.md)) keeps
application-owned state under an application-defined key, with an optional
stable id that is safe to hold after the flow is gone. It does not say what a
flow is: a five-tuple, a tunnel id, a subscriber, a MAC and a domain are all
just a trivially copyable `Key`. It has no dependency on `Module`, gates, the
runtime, control or protobuf; reaching it from a module is the module's job.

**Experimental API.** The headers other than `shared_flow_table.h` are installed
and may change without preserving source compatibility (architecture.md
section 5). `shared_flow_table.h` is not installed because it is built on
`ConcurrentExactTable`, which is internal.

## Which table

| situation | use |
|---|---|
| packet steering gives each flow one owning worker | `WorkerFlowTable`, one per worker. Nothing is atomic; State is plain loads and stores. |
| any worker must classify any flow, state mutated by the creating worker | `SharedFlowTable` with `StateSharing::kOwnedByCreator` |
| any worker may mutate any flow's state | `SharedFlowTable` with `StateSharing::kSharedMutable`; `State` brings its own atomics or lock |
| new flows are frequent and cannot be steered | partition (`WorkerFlowTable` per worker) and hand creation to the owner (D-028, D-019); one shared writer lock collapses under contention |

The first and the last rows are the same shape. Decision D-028 has the numbers
for why a single shared writer lock is only right when new flows are rare.

## WorkerFlowTable

```cpp
#include "flow/worker_flow_table.h"
using namespace bess::flow;

struct Key { uint32_t src, dst; uint16_t sport, dport; uint8_t proto, pad[3]; };  // pad always zero
struct Conn { uint64_t last_seen; uint32_t state; };

struct NatTraits : DefaultFlowTableTraits {
  static constexpr size_t kAliases = 1;         // the reverse key
  using Observer = MyExpiry;                    // optional, see below
};
using Table = WorkerFlowTable<Key, Conn, DefaultFlowHash<Key>,
                              DefaultFlowEqual<Key>, NatTraits>;

auto table = Table::Create(1 << 20);            // std::expected<unique_ptr<Table>, FlowTableError>
auto made = (*table)->EmplaceAliased(fwd, rev, now, 0);   // State(now, 0) built in place
Conn *c = (*table)->Find(rev);                  // the same State, found by either key
FlowHandle h = made.handle;                     // 8 bytes; keep it, not the pointer
(*table)->Lookup(h);                            // nullptr once the flow is erased
(*table)->Erase(h);                             // false if h is stale
```

- **Capacity is fixed at `Create`.** It allocates everything there (and touches
  it: the memory is committed at creation) and never again: no resize, no
  rehash, no allocation on any other call. `Create` reports
  `kInvalidCapacity`, `kTooLarge` or `kOutOfMemory` and leaves nothing
  allocated. `Traits::Allocator` is the seam for a hugepage or NUMA-aware
  allocator; the default is aligned `operator new`.
- **A full table says so.** `Emplace` returns `EmplaceStatus::kFull`, a null
  `state` and changes nothing; the observer hears `OnFull()`. The table never
  evicts. What to do (drop, evict an expired flow, count) is the application's;
  `Erase` frees exactly one slot. `SharedFlowTable` has a second refusal,
  `kPlacementFailed`, for a key its directory cannot place although slots are
  free (see below).
- **Duplicates.** `Emplace` of a present key returns `kExists` with the existing
  flow and does not construct a State.
- **State** is constructed in place and never moved or copied: it needs no move
  constructor, may hold atomics, and its address is stable until the flow is
  erased. If its constructor throws, the table is unchanged.
- **Key, hash, equality.** `Key` must be trivially copyable (`FixedFlowKey`).
  The default hash and equality read the key's bytes and are only available when
  those bytes are canonical: no padding (`std::has_unique_object_representations`),
  or a `FlowKeyTraits<Key>` specialisation in which the author promises every
  `Key` object has zeroed padding. A padded struct without either fails to
  compile with a message saying so. Keys with a mask, a don't-care field or a
  case-insensitive part take an explicit `Hash` and `Equal` (both `noexcept`).
  The table mixes whatever hash it is given, so `std::hash<uint64_t>`-style
  identity hashes are safe.
- **Batches.** `FindBatch(keys, out)` (up to 64) requests the index line of every
  key before reading any, so a batch overlaps its cache misses. It returns a hit
  mask; `out[i]` is the State or null. `Traits::kPrefetchSlots = true` also
  requests the slot record of each candidate hit: it helps all-hit batches over
  a table far larger than the caches and costs on misses and cache-resident
  tables (numbers in D-052), so it is off by default.
- **Reuse policy.** `Traits::kReuse`: `kLifo` (the slot freed last; warm in
  cache) or `kFifo` (the slot freed longest ago; a slot's generation advances
  1/capacity as fast). Use FIFO when handles live long (continuations, hardware
  marks).

### FlowId, FlowHandle and generations

`FlowId` is one-based (zero is "no flow"). A `FlowHandle` is a `FlowId` plus
the slot's generation, 8 bytes, compared as one word. A slot's generation is odd
while it holds a flow and even while free; it advances at creation and at
erasure, so a handle matches only the lifetime it was issued for, and a forged
handle with an even generation matches nothing.

`Lookup(handle)`, `Erase(handle)`, `KeyOf(handle)`, `AddAlias(handle, ...)`
fail closed on a stale handle: it never reaches the flow that reused the slot.
A slot whose generation counter is exhausted (2^31 reuses of that one slot) is
retired, not reused (`quarantined_slots()`), so a handle can never be matched by
a later lifetime; under LIFO reuse a hot slot reaches that after minutes of
flat-out churn, under FIFO after `capacity` times as long. (A `SharedFlowTable`
create that is refused for placement, or whose State constructor throws, also
moves its slot one lifetime on, for the reason given below; it is the same wear,
and under FIFO a stream of refusals spreads it over the free slots.)

### Aliases

`Traits::kAliases = N` lets each flow carry up to N extra keys that find the same
State: the reverse key of a bidirectional flow, a second tunnel id. Each alias is
another index entry pointing at the same slot (the representation D-052
chose; see the decision for the alternatives). `FindRef(key)` also says whether
the key was an alias (`via_alias`), which is how an application tells forward
from reverse without the table knowing about directions.

- `EmplaceAliased(key, alias, args...)` creates both keys or neither.
  `WorkerFlowTable` is single-threaded, so that is all it can mean.
  `SharedFlowTable` makes it true for concurrent readers too: both keys become
  visible at one instant (see below).
- `AddAlias(handle, alias)`: `kAdded`, `kStale`, `kExists` (the key is taken, by
  any flow), `kNoRoom`; `SharedFlowTable` also has `kPlacementFailed`.
- `RemoveAlias(key)` removes one alias; the primary key cannot be removed this
  way.
- `Erase(key or handle)` removes the flow and all its keys together. No alias
  outlives its flow.

### Ownership diagnostics

A worker-owned table is unsynchronised, so touching it from a second thread is a
data race nothing else will report. `Traits::Owner` is a policy with
`kChecked` and `static OwnerToken Current()`. With `kChecked`, every call
compares `Current()` with the owner (bound on the first call) and aborts with a
message on a mismatch; without it nothing is stored or compiled. The default is
`ThreadOwner` in builds without `NDEBUG` and `UncheckedOwner` with it. The flow
library may not include `worker.h`, so worker identity is injected:

```cpp
struct WorkerOwner {
  static constexpr bool kChecked = true;
  static OwnerToken Current() noexcept { return TokenOf(bess::CurrentWorkerId()); }
};
```

`ReleaseOwner()` hands a table to another worker (call it from the owner, after
the worker has stopped using it).

### Expiry: the observer seam and `ExpiryWheel`

`Traits::Observer` is a type with `OnCreate(handle, state)`,
`OnErase(handle, state)` and `OnFull()`, called inline (no virtual call, no
`std::function`; `NoFlowObserver` is empty). The expiry engine of milestone M10
(`dataplane/expiry_wheel.h`, guide in [expiry.md](expiry.md)) plugs in through
it: schedule in `OnCreate`, cancel in `OnErase`, keep the `FlowHandle` as the
timer's payload, and expire by calling `table.Erase(handle)` from the poll
callback. Because the handle carries the generation, a record that outlived its
flow cannot erase the flow that reused the slot. The table has no timer and no
`Touch()`: a refresh is either `ExpiryWheel::Refresh` or a plain store into the
application's State (both are shown, tested against a model, in
`core/flow/expiry_consumer_test.cc`). The flow library does not include the
engine and the engine does not include the flow library. `FlowCounters` is an
optional ready-made observer that counts creates, erases and refusals.

## SharedFlowTable

The directory is a `ConcurrentExactTable` (DPDK `rte_hash`: lock-free readers,
QSBR-deferred delete) holding the 8-byte `FlowHandle` of each key; the State
lives in the library's own slot array, so it may be any size and keeps its
address. Readers (`Peek`, `Find`, `FindBatch`, `Lookup`, from any thread) take no
lock and do no atomic read-modify-write. Writers (`Emplace`, `Erase`, aliases)
may come from any thread and are serialized by one spinlock.

- **A flow appears, and disappears, at one instant for all its keys.** The
  directory's value for each key of a flow is the flow's live handle, and every
  reader path (`Peek`, `Find`, `FindOwned`, `FindHandle`, `PeekBatch`,
  `FindBatch`) accepts a directory value only if the slot's generation (one
  acquire load) still equals the handle's and is live. `Emplace` and
  `EmplaceAliased` put every key into the directory first, build the State, and
  then release-store the live generation; until that store nothing resolves, and
  after it everything does, with the State and keys visible. A reader that
  found one key of an aliased flow and then looks for the other finds it too,
  with the same handle, unless the flow has been erased in between. `Erase`
  mirrors it: the generation moves on first (no key resolves from then on), and
  the keys leave the directory after. A directory value that went stale, from an
  erased flow or an abandoned create, never resolves (D-056 has the reasoning).
- **A key the directory cannot place is a refusal, not an abort.** Free capacity
  does not promise that a given key fits: the directory is an `rte_hash` of
  eight-entry buckets, and keys that share a bucket pair fill it at sixteen
  however empty the table is. Flow keys come from packets, so this can be
  provoked. `Emplace` and `EmplaceAliased` return `EmplaceStatus::kPlacementFailed`
  and `AddAlias` returns `AliasStatus::kPlacementFailed`, and the table holds the
  same flows, keys and size as before; no State is built (its constructor does
  not run and its arguments are not consumed), no `OnCreate` fires, and a
  refused create tells the observer `OnFull()` (the table had no room for the
  flow; the status says which room). An alias that had already reached the
  directory is taken back out. The caller treats it like `kFull` (drop, count,
  expire something and try again); retrying the same key succeeds only after
  something in its bucket pair has been erased.
  `FindHandle`, `Find` and the rest are unaffected.
- An erased flow stops resolving at once, but its State is destroyed and its
  slot reused only after a grace period of the `RcuDomain` the table was created
  with, so a worker that found the State before the erase may use it until its
  next quiescent state.
- Destruction runs under the writer lock on a thread that calls `Reclaim()` or
  that calls `Emplace` when no slot is free. A control thread calling
  `Reclaim()` periodically keeps destructors off workers.
  `pending_reclaim()` is the backlog; a stalled reader makes it grow until the
  table is full.
- A refused create keeps a directory entry for a moment (it is inserted, then
  taken back), and a deleted directory key waits out a grace period in the
  directory like an erased flow's does. A stream of refusals that each insert and
  take back an alias therefore spends the directory's headroom (5% of its slots,
  at least 256) until readers pass a quiescent state, as a stream of erases does;
  the consequence is more refusals, never a fault.
- `StateSharing::kSharedMutable`: `Find`/`Lookup` return `State *`; State must
  synchronise itself. `kOwnedByCreator`: other threads get `const State *` from
  `Peek`; `FindOwned`/`LookupOwned` return `State *` and, under a checked
  `Traits::Owner`, abort if the caller is not the thread that created the flow.
- The table must be destroyed after the workers stopped using it.
- The table's reader-side fields (directory pointer, slot array, capacity) are
  on a cache line of their own, and the counters, lock and observer that every
  create and erase writes are on the next, so a writer does not invalidate the
  line every lookup loads (D-056 has the measurement).
- `Traits::Hook` (default `NoSharedFlowTableHook`, empty, no code) is a test
  seam called inside `Erase` between the generation store and the first
  directory erase; the tests use it to prove that every key already resolves to
  nothing there. Applications leave it alone.

## DecisionCache

```cpp
#include "flow/decision_cache.h"

DecisionGeneration policy;                       // one per policy scope, shared
auto cache = DecisionCache<Tuple, MyDecisionId>::Create(1 << 16, policy).value();

MyDecisionId id;
switch (cache->Lookup(key, &id)) {
  case DecisionLookup::kHit:   /* resolve id in the application's table */ break;
  case DecisionLookup::kMiss:
  case DecisionLookup::kStale: {
    const uint64_t g = policy.Current();       // read before compiling
    id = Compile(key);                          // or punt the key (M11)
    cache->Install(key, id, g);                 // refused if policy moved on
  }
}
// Control side: publish the new policy first, then invalidate (O(1)).
PublishNewPolicy();
policy.Invalidate();
```

A `WorkerFlowTable` whose State is `{DecisionId, generation}` (M12, D-062). The
cache knows nothing about what a decision is: `DecisionId` is the application's
id type, resolved in the application's own table (typically an RCU-published
`SlotTable`). A hit is one flow lookup and one compare with the policy scope's
current generation; `LookupBatch` reads the generation once per batch.

- **Invalidation is O(1).** `DecisionGeneration::Invalidate()` is one increment;
  every entry compiled against an older generation reads as `kStale`. The
  generation is 64 bits and never wraps.
- **Install is checked against the generation the decision was compiled for.**
  A policy change between reading the generation and installing makes the
  install `kStaleGeneration`, so a decision compiled against old policy is never
  installed as current -- if the control side publishes the new policy before
  calling `Invalidate()`. Invalidating first lets a worker read the new
  generation, compile against the old policy and install it as current.
- **Stale entries keep their slot** until their key is installed again (reused
  in place) or erased; keys that do not come back leave through `Erase`,
  typically driven by `ExpiryWheel`. There is no sweep. A cache full of stale
  entries refuses new keys (`kFull`).
- **Ownership:** one worker owns a cache (it is a `WorkerFlowTable`); any thread
  may invalidate. Decision objects are published and retired through RCU, since
  a worker may hold an id across a policy change until its next quiescent state.
- There is no shared (multi-worker) variant yet: a steerable workload gives each
  worker its own cache under one shared generation.

## Memory

Per flow, at full occupancy:

```
WorkerFlowTable  slot record + 4 (free list) + 16 x (1 + aliases) (index)
SharedFlowTable  slot record + 4 (free list) + 4 (reclaim ring) + the rte_hash directory
slot record      (1 + aliases) x sizeof(Key) + 8 (generation, key mask) + sizeof(State), aligned
```

`WorkerFlowTable::memory_bytes() / capacity()` reports it. Measured values per
key, State and table kind are in D-052. The index is sized for 4 entries per
64-byte bucket (50% load), chosen because deletion never moves an entry back and
a higher load makes a missing lookup walk several buckets under churn.

## Not provided

No timers or aging inside the table (see above: they are `dataplane/expiry_wheel.h`, wired through the observer), no eviction policy, no TCP or NAT semantics, no
resizing, no per-flow counters unless the observer adds them, no NUMA or hugepage
allocator beyond the `Traits::Allocator` seam. Behaviour at 10 million flows was
not measured (D-052).
