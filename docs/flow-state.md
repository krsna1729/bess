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
  `Erase` frees exactly one slot.
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
flat-out churn, under FIFO after `capacity` times as long.

### Aliases

`Traits::kAliases = N` lets each flow carry up to N extra keys that find the same
State: the reverse key of a bidirectional flow, a second tunnel id. Each alias is
another index entry pointing at the same slot (the representation D-052
chose; see the decision for the alternatives). `FindRef(key)` also says whether
the key was an alias (`via_alias`), which is how an application tells forward
from reverse without the table knowing about directions.

- `EmplaceAliased(key, alias, args...)` creates both keys or neither.
- `AddAlias(handle, alias)`: `kAdded`, `kStale`, `kExists` (the key is taken, by
  any flow), `kNoRoom`.
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

### The expiry seam (for M10)

`Traits::Observer` is a type with `OnCreate(handle, state)`,
`OnErase(handle, state)` and `OnFull()`, called inline (no virtual call, no
`std::function`; `NoFlowObserver` is empty). The timer substrate of milestone
M10 will implement it: schedule in `OnCreate`, cancel in `OnErase`, keep
`FlowHandle`s in its records, and expire by calling `table.Erase(handle)`. Because
the handle carries the generation, a record that outlived its flow cannot erase
the flow that reused the slot. The table has no timer and no `Touch()`: a
refresh is a plain store into the application's State. `FlowCounters` is an
optional ready-made observer that counts creates, erases and refusals.

## SharedFlowTable

The directory is a `ConcurrentExactTable` (DPDK `rte_hash`: lock-free readers,
QSBR-deferred delete) holding the 8-byte `FlowHandle` of each key; the State
lives in the library's own slot array, so it may be any size and keeps its
address. Readers (`Peek`, `Find`, `FindBatch`, `Lookup`, from any thread) take no
lock and do no atomic read-modify-write. Writers (`Emplace`, `Erase`, aliases)
may come from any thread and are serialized by one spinlock.

- An erased flow leaves the directory at once and its handle stops resolving at
  once, but its State is destroyed and its slot reused only after a grace period
  of the `RcuDomain` the table was created with, so a worker that found the State
  before the erase may use it until its next quiescent state.
- Destruction runs under the writer lock on a thread that calls `Reclaim()` or
  that calls `Emplace` when no slot is free. A control thread calling
  `Reclaim()` periodically keeps destructors off workers.
  `pending_reclaim()` is the backlog; a stalled reader makes it grow until the
  table is full.
- `StateSharing::kSharedMutable`: `Find`/`Lookup` return `State *`; State must
  synchronise itself. `kOwnedByCreator`: other threads get `const State *` from
  `Peek`; `FindOwned`/`LookupOwned` return `State *` and, under a checked
  `Traits::Owner`, abort if the caller is not the thread that created the flow.
- The table must be destroyed after the workers stopped using it.

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

No timers or aging (M10), no eviction policy, no TCP or NAT semantics, no
resizing, no per-flow counters unless the observer adds them, no NUMA or hugepage
allocator beyond the `Traits::Allocator` seam. Behaviour at 10 million flows was
not measured (D-052).
