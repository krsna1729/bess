# Dataplane tables: options for module authors, and why they are what they are

This page is for anyone writing or changing a BESS module that looks things up
on the packet path: rules, routes, flows, MAC addresses, meters, counters. It
covers:

- which structures exist and when to pick each one;
- how changes reach the workers that are reading;
- which built-in module uses what;
- where to find the decisions behind the current set;
- the DPDK behaviours we depend on, and the tests that fail if they change.

The default authoring model is unchanged. A module that keeps plain members
and marks its commands `THREAD_UNSAFE` still works: BESS pauses workers
around the command, exactly as it always has. Everything below is opt-in, for
modules whose tables are large, change often, or must change without
stopping traffic.

## 1. Rules every shared table follows

1. **Read once per batch.** A worker loads the table (or its published
   generation) at the start of `ProcessBatch` and uses that one view for the
   whole batch. It never caches the view across invocations: the view stays
   valid only until the worker's next quiescent point, which is between task
   invocations.
2. **Workers only read shared tables.** Writes come from the command thread
   (module commands, the v2 control API). State that the packet path itself
   writes, such as learned flows or counters, is worker-owned instead
   (section 3).
3. **Published state is never mutated in place, except where a structure is
   built for it.** `RcuPtr` generations are immutable once published. The
   exceptions are `ConcurrentExactTable` and `RouteTable`: DPDK designed them
   for one writer changing them in place under lock-free readers, and they
   are wired to the same grace periods (QSBR) as everything else.
4. **Memory is freed only after a grace period.** Every structure hands
   retired memory (old generations, deleted hash slots, freed LPM groups) to
   the runtime's single `RcuDomain`. The domain frees it once every worker has
   passed a quiescent point. Workers report one every 10 µs of scheduler
   time, so a grace period lasts about max(10 µs, the longest single task
   invocation): p99 about 11 µs for ordinary pipelines (D-012). State owned
   by one worker (mode W) needs no grace period at all (D-013). Nothing on
   the packet path takes a lock, counts references or frees memory.

## 2. Choosing a structure

| you need | use | header | updated by |
|---|---|---|---|
| exact match on runtime-defined fields (packet/metadata bytes → small value) | `ConcurrentExactTable` | `classifier/concurrent_exact.h` | in place, O(1) (mode C) |
| masked/ternary match with priorities (5-tuple ACL style) | `RuntimeMaskedBackend` in a generation | `classifier/masked_exact.h` | rebuild + swap (mode G) |
| exact match with a compile-time key type (a struct you own) | `ExactTable<Key, Result, Backend>` | `classifier/typed_exact.h` | rebuild + swap (G) |
| IPv4 longest-prefix match | `RouteTable<Value>`; `Router` for routes + next hops | `route/route_table.h`, `route/router.h` | in place (C) |
| id → object (the result of a lookup is a rich object) | `ObjectTable<Id, T>` | `dataplane/object_table.h` | rebuild + swap (G) |
| rate limiting / policing | `MeterSet` (+ `MeterState`) | `meter/meter_set.h` | set: G; bucket state: per meter, see header |
| counters and histograms read by the controller | `CounterSet`, `WorkerHistogram` | `stats/counter_set.h`, `stats/worker_histogram.h` | worker-owned |
| any other per-worker scratch | `WorkerLocal<T>` | `stats/worker_local.h` | worker-owned |
| flows the packet path creates (NAT-style learning) | `utils::CuckooMap` owned by one worker | `utils/cuckoo_map.h` | worker-owned |
| any immutable configuration object | `RcuPtr<T>` | `rcu/rcu_ptr.h` | G |
| turning packet bytes into a lookup key | `ExtractPlan` | `classifier/extract_plan.h` | compiled with the generation |

When in doubt:

- For exact match, use `ConcurrentExactTable`.
- For a small configuration that rarely changes, use `RcuPtr` with an
  immutable struct.
- For anything workers write, use a worker-owned structure.

## 3. How changes reach the workers (update modes)

| mode | what happens on a change | cost of one change | packet path | use for |
|---|---|---|---|---|
| **Pause** (the default) | the command is marked `THREAD_UNSAFE`; BESS pauses **every** worker, runs the command, resumes | a pause/resume of all workers | untouched, but traffic stops during the command | simple modules, rare changes |
| **G: generation swap** | build a new immutable generation off the packet path, `RcuPtr::Publish` it, retire the old one after a grace period | proportional to the **whole table** | one acquire load per batch | small tables; bulk loads and restores; structures with no incremental update (e.g. `rte_acl`) |
| **C: concurrent in place** | the command thread changes a shared table that DPDK designed for one writer with lock-free readers; freed memory waits for a grace period | **O(1)**, independent of table size | the table's own lock-free lookup | large tables that change often: exact match, routes |
| **worker-owned** | only the owning worker writes; the controller reads via snapshots or seqlocks | a store to the worker's own cache line | no sharing at all | learned flows, counters, histograms |
| **W: per-worker op rings** (planned) | the command thread queues ops to each worker; the worker applies them between scheduler rounds into its own replica or shard | O(1) per worker | none: the worker's own table | when one writer is not enough, or state is sharded per worker |

Choosing between G and C comes down to how the cost of one change grows with
the table. Generation swap rebuilds everything. That is fine at tens of rules
and fatal at millions:

| structure | one change by rebuild (G) | one change in place (C) |
|---|---|---|
| ExactMatch, through the module command | 426 µs at 1K rules, 47 ms at 100K, 645 ms at 1M | 0.3 µs at any size |
| IPLookup (`rte_lpm`) | 9.4 ms at 1K routes, 341 ms at 64K; a 512K build takes ~21 s | 0.5-33 µs |
| WildcardMatch (masked, 8 tuples) | 7 µs at 8 rules, 500 µs at 256 rules | (G today; see section 5) |

A whole-table swap is reserved for cases with no incremental alternative: a
restore (`set_runtime_config`), `Clear()` on a route table, and structures
such as `rte_acl` that can only be compiled whole.

Marking a command `THREAD_SAFE` is a promise that it is safe to run while
workers keep processing packets. Only mark it so when the command changes
state through G, C or W.

## 4. The structures

### `ConcurrentExactTable` (exact match, mode C)

- **What:** DPDK `rte_hash` created with `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF`,
  attached to the runtime QSBR in defer-queue mode (`rte_hash_rcu_qsbr_add`).
  Keys have a fixed runtime length; values are up to 8 bytes, stored in
  `rte_hash`'s data pointer.
- **Writer API** (the command thread, serialized by the caller):
  - `Upsert(key, value)` → `kInserted` / `kUpdated` / `kFull`;
  - `Erase(key)`;
  - `ForEach(fn)`;
  - `Reclaim()`;
  - `size()` (live entries) and `slots_in_use()` (live plus deleted slots
    still waiting out a grace period).
- **Reader API:** `LookupBatch(keys, stride, values, n)` for up to 64 keys.
  It returns a hit mask; misses leave `values[i]` untouched.
- **Capacity:** fixed at creation, in key slots. The sizing policy is
  [D-010](decisions.md#d-010-size-concurrent-tables-for-occupancy-and-grace-period-headroom-not-a-fixed-75):
  - create with `CapacityFor(rules)`: 3/4 of a power of two, so every slot
    is reachable;
  - before an add, check `HasRoomForOne()`. Live keys plus deletes still
    waiting out a grace period must leave `Headroom()` slots free: the
    larger of 5% and 256, which covers one writer's peak rate over a 64 µs
    grace period;
  - when there's no room, copy into a table twice the size and publish that
    (amortized O(1));
  - if an add returns `kFull` anyway, grow instead of failing.
  - `ExactMatch::EnsureCapacity`/`FillTable` is the reference
    implementation.
- **Guarantees to readers:**
  - a lookup sees the old or the new value of a key, never a mix: an update
    swaps the value atomically;
  - a deleted key's slot is never reused while a reader could still be
    reading it.
- **Used by:** ExactMatch.

### `RuntimeMaskedBackend` (masked/ternary match, mode G)

- **What:** tuple-space search. There is one exact table per distinct mask,
  and a lookup masks the batch once per tuple, looks it up, and merges by
  priority.
  - Rules must be canonical (`value & ~mask == 0`); anything else is
    rejected at build.
  - Equal priorities resolve to the later rule.
- **Updates:** immutable once built; published in a generation through
  `RcuPtr`.
- **Used by:** WildcardMatch.

### Typed `ExactTable<Key, Result, Backend>` (compile-time keys, mode G)

- **What:** a typed front end for authors who have a real key type rather
  than runtime-defined fields. Hashing must be opted into (`KeyTraits`, or an
  explicit `Hash`/`Equal`), so a struct's padding is never hashed by
  accident.
- **Backends:**
  - `CuckooExactBackend`, the general case;
  - `SmallExactBackend`, a linear scan for ≤ 64 rules;
  - `DirectExactBackend`, an array for 1- or 2-byte keys;
  - `RteHashPositionBackend` / `RteHashDataBackend`, `rte_hash` without
    concurrency flags.

  All are immutable after build.
- **Status:** this set came out of the K3 backend experiments and is due to
  be consolidated ([D-009](decisions.md#d-009-consolidate-the-exact-match-backends)). New code that needs exact match with runtime
  updates should use `ConcurrentExactTable`.

### `RouteTable<Value>` and `Router` (IPv4 LPM, mode C)

- **What:** DPDK `rte_lpm` changed in place by one serialized writer, with
  lock-free readers. Freed tbl8 groups are recycled only after a QSBR grace
  period (`rte_lpm_rcu_qsbr_add`, defer queue). The `/0` route is kept beside
  the table as an atomic default.
- **Semantics:**
  - each route change is atomic to readers;
  - a sequence of changes is not one transaction;
  - `Clear()` builds a fresh table and swaps it.
- **`Router`:** adds next hops (`NextHopId` → `NextHop`) in an
  `ObjectTable`, so a neighbour change republishes only the next-hop table.
  Ordering is enforced:
  - a route can only name an existing next hop;
  - readers fence between the route and next-hop loads;
  - a next hop cannot be removed while any route names it;
  - removing a next hop does not wait. The id stays published and
    unusable ("retiring") until readers pass a grace period, and later
    control calls drop it.
- **Used by:** IPLookup.

### `ObjectTable<Id, T>` (id → object, mode G)

- **What:** an immutable dense array indexed by a strongly typed, one-based
  id, for when a lookup result is better carried as a compact id than as an
  inline value (next hops, actions, sessions).
- **Id semantics:**
  - ids are stable across generations;
  - erasing leaves a hole, which is never refilled automatically, because
    reusing an id is an ABA hazard that RCU does not solve;
  - who allocates ids decides reuse.

### `MeterSet` (metering)

- **What:**
  - DPDK `rte_meter` owns the algorithms: srTCM, trTCM and trTCM-4115.
  - BESS owns profiles, stable ids, where bucket state lives, and
    generation-safe publication.
  - A meter's state survives generations, so publishing a change to one
    meter does not refill every bucket.
- **Concurrency:** each meter declares who may check it:
  - `kWorkerExclusive` meters are unsynchronized, and running one from two
    workers is a caller bug;
  - `kShared` meters take the meter's own spinlock on each check. That is
    correct, but it serializes contending workers on one cache line.

  See `meter/meter.h`.

### `CounterSet`, `WorkerHistogram`, `WorkerLocal<T>` (worker-owned)

- **What:**
  - Each worker writes only its own cache lines, with no locked
    instruction.
  - The controller takes snapshots; a sequence word keeps a group of
    counters from being read half-updated.
  - Reset never writes worker cells: it records a baseline instead.
- **Use for:** anything the packet path counts.

### `utils::CuckooMap` (worker-owned mutable hash)

- **What:** BESS's original single-writer cuckoo hash, with prefetch hooks
  for batch lookups.
- **Use:** only as state owned and written by one worker (NAT's flow table).
  It is not safe for a command thread, or a second worker, to write while a
  worker reads.

### `ExtractPlan` (packet → key)

- **What:** compiles a key layout (packet offsets, metadata attributes, masks)
  once per generation and extracts a batch of keys with bounds checks. Short
  or multi-segment packets fail extraction, and the module sends them to its
  default gate instead of reading past the data.

## 5. Built-in modules today

| module | table | how changes apply | notes |
|---|---|---|---|
| ExactMatch | `ConcurrentExactTable` | C: add/delete/clear in place; default gate and restore by G | 0.3 µs per add at any size |
| IPLookup | `RouteTable` (`rte_lpm`) | C | |
| WildcardMatch | `ConcurrentMaskedTable` (one `ConcurrentExactTable` per mask) | C: add/delete in place; a new or vanished mask republishes only the tuple list | D-014 |
| L2Forward | `l2_table` (inline 4-way buckets) | C: single-writer, lock-free readers; whole-word slot stores, no grace period | D-017 |
| ACL | `std::vector` of rules, linear scan | G: `add` copies, appends and publishes (all or nothing) | D-017; `rte_acl` (G-only) is the candidate for large rule sets |
| HashLB | configuration only (`ExactMatchTable` for field layout) | G: one `RcuPtr<Config>`, read once per batch | D-017 |
| URLFilter | `Trie` per host | Pause | legacy: cleartext HTTP only; a modern SNI classifier is recorded in MODERNIZATION §31.6 |
| BPF | compiled filters | Pause | next: G, with the move to `rte_bpf` |
| NAT | `CuckooMap` | worker-owned (the packet path learns flows) | limited to one worker |
| DRR | `CuckooMap` of flows | written by the packet path | **open issue:** DRR allows several workers, yet its `ProcessBatch` writes the flow map with no synchronization; to review |

## 6. Why it is like this

The reasoning, rejected alternatives, evidence and revisit triggers are in
[decisions.md](decisions.md):

- [D-001](decisions.md#d-001-exact-match-lock-free-rte_hash--qsbr-updated-in-place)
  exact match on lock-free `rte_hash`, with its lookup cost against the old
  table;
- [D-002](decisions.md#d-002-rte_hash-flags-lf-qsbr-defer-queue-not-multi-writer-not-ext-table)
  which `rte_hash` modes, and why;
- [D-003](decisions.md#d-003-ipv4-lpm-rte_lpm-updated-in-place-rte_fib-rejected)
  `rte_lpm` in place, and `rte_fib` rejected;
- [D-004](decisions.md#d-004-update-modes-concurrent-c-and-per-worker-w-generation-swap-g-only-for-bulk-loads)
  update modes;
- [D-005](decisions.md#d-005-control-ingress-keep-grpc-with-a-streaming-packed-record-api)
  control transport;
- [D-006](decisions.md#d-006-batch-lookup-body-staged-or-plain-chosen-per-table)
  staged vs plain lookups;
- [D-007](decisions.md#d-007-dpdk-behaviours-we-depend-on-are-deterministic-ci-tests)
  how DPDK behaviours are pinned;
- [D-009](decisions.md#d-009-consolidate-the-exact-match-backends)
  the exact-match backend consolidation (open);
- [D-010](decisions.md#d-010-size-concurrent-tables-for-occupancy-and-grace-period-headroom-not-a-fixed-75)
  table sizing and grace-period headroom;
- [D-011](decisions.md#d-011-tune-per-table-at-build-time-from-host-facts-discovered-once-per-process)
  what is tuned at daemon start and what per table (section 6a).

## 6a. What is tuned, and when

BESS discovers host facts once per process, and decides everything that
depends on a particular table when that table is built. Nothing is tuned
while packets flow except NAT's per-batch prefetch check (below), and
nothing needs privileges.

**Once per process (the daemon logs these at startup):**

| fact | how | used by |
|---|---|---|
| cache sizes (L1d/L2/L3, line) | `CacheGeometry::Smallest()`: each online CPU's `/sys/devices/system/cpu/cpuN/cache`, falling back to `sysconf`, then defaults; the minimum over all CPUs, because which CPU will run a table is not known when it is built | every table-build decision below |
| lookup-body override | `BESS_LOOKUP_BODY=plain\|staged`, read once (`LookupBodyOverride()`) | forces every `kAuto` choice, for experiments |
| DPDK's CRC implementation | DPDK selects SSE4.2 / ARMv8 CRC at EAL init | `rte_hash`, and every CRC-hashed table |

**When a table is built or created:**

| table | decided | how |
|---|---|---|
| cuckoo backends (WildcardMatch tuples, typed `ExactTable`) | plain or staged body, at generation build | `ResolveLookupBody()` from the built table's footprint and lookup shape |
| L2Forward's `l2_table` | plain or staged body, at `l2_init` (module Init) | same |
| NAT's `CuckooMap` | whether to prefetch, **per batch** | `PrefetchBatch()` re-checks the footprint each batch, because the table grows while packets flow; the check is a size comparison |
| `ConcurrentExactTable` (ExactMatch) | capacity, at create and on growth | `CapacityFor(rules)` and `Headroom()` (D-010); DPDK's own bulk lookup, no body choice |
| `ConcurrentExactTable`, inside `rte_hash` | signature and key compare functions, at create | DPDK picks SSE2 signature compare, and a SIMD key compare for 16/32/…/128-byte keys (`memcmp` otherwise) |
| `RouteTable` (`rte_lpm`) | nothing | always plain: one independent load per packet |

**Not done, deliberately:**

- *No start-up micro-benchmark calibration.* Cache sizes from sysfs were
  enough to reproduce the measured plain/staged crossovers.
- *No per-worker geometry.* The smallest CPU is the safe choice when a table
  may be read by any worker.
- *No retuning when workers move or the host changes load.* Revisit if
  tables become pinned to known workers (mode W shards), since then the
  owner's actual CPU is known.

See D-006 and D-011.

## 7. DPDK behaviours we depend on, and the tests that pin them

Each row is a regular unit test that runs in CI. If a DPDK upgrade changes the
behaviour, the test fails. Each test was checked once when written: removing
the protection it guards made it fail, 3 runs out of 3.

| behaviour | test |
|---|---|
| `rte_hash` LF + QSBR DQ: a deleted slot is not reused while a reader that was online before the delete is still unreported; it returns once the reader is quiescent | `ConcurrentExactTableTest.ErasedSlotWaitsForOnlineReaders` |
| `rte_hash`: re-adding an existing key swaps its value in place and takes no new slot | `ConcurrentExactTableTest.InsertUpdateEraseAndIterate` |
| `rte_lpm` + QSBR DQ: a freed tbl8 group is not handed to another /24 mid-grace-period; the next add reclaims it after quiescence | `RouteTableTest.FreedTbl8GroupWaitsForOnlineReaders` |
| `rte_lpm`: arbitrary-order inserts and churn match an independent reference | `RouteTableTest.LargeArbitraryOrderMatchesReferenceThroughChurn` |
| `rte_meter`: our state wrapper colours exactly as `rte_meter` does | `MeterDifferentialTest.MatchesRteMeter` |
| `rte_rcu_qsbr` via `RcuDomain`: grace periods wait for every online reader, not offline ones | `RcuDomainTest.*` (`rcu/rcu_test.cc`) |
| `rte_mbuf` private-area layout | `static_assert`s in `packet.h` |

The stress tests (`...ConcurrentReadersOnlySeeJustifiedAnswers`,
`RouterTest.ConcurrentChurnNeverLosesANextHop`) remain as extra coverage. No
guarantee rests on them alone: a race window a few instructions wide can
pass a stress run by luck.

## 8. Adding a table to a module: checklist

- Pick the structure and update mode from sections 2 and 3.
- Read the table once per batch; do not cache the view across invocations.
- Write only from commands; mark a command `THREAD_SAFE` only if it goes
  through G, C or W.
- For C: handle `kFull` by growing, size spare slots for rate × grace
  period, and keep one writer.
- For G: build off the packet path, publish with `RcuPtr::Publish`, and never
  touch the published object again.
- For concurrency, write a deterministic test of the invariant (a reader
  held online, then released) rather than relying on a stress test. Check it
  once by removing the protection and watching it fail.

## 9. Re-measuring

Run a benchmark pinned to one isolated core. The tables above name each one.

| decision | benchmark |
|---|---|
| exact-match backend and its insert cost | `modules_exact_match_update_bench` (`BM_Lookup`, `BM_ModuleAddDelete`) |
| occupancy before a failed add, add cost as tables fill, churn failures versus grace periods | `occupancy_bench [fill\|churn\|all] [max_log2_entries]` |
| update-mode thresholds, writer rate | `update_scale_bench` |
| control-ingress transport | `ingress_bench` |
| plain vs staged body rule | `modules_table_scale_bench`, `classifier_cuckoo_scale_bench` |
| `rte_fib` rejection (correctness gate) | `fib_bench` with `BESS_FIB_GATE=1` |
| LPM lookup and update cost | `route_bench` |
| meters, counters, RCU | `meter_bench`, `stats_bench`, `rcu_bench` |
