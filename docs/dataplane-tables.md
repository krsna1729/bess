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
3. **Resolve metadata offsets per batch, and fail closed.**
   `Module::Init()` registers metadata attributes; it does not assign
   graph-relative offsets. `ComputeMetadataOffsets()` assigns offsets from the
   connected graph.
   `PauseAll()` stops workers but does not compute the layout. `ResumeAll()`
   invokes `SetupMetadata` before `resume_all_workers()` but does not pause
   workers itself. `bessctl run file` calls `resume_all()` in its `finally`
   block. Do not call `resume_all()` inside the script: that starts workers;
   the wrapper then runs `SetupMetadata` again while they can be reading the
   offsets, which are rewritten in place (D-032). Keep workers quiescent for
   layout computation. Check `bess::metadata::IsValidOffset()` and drop or
   take a default path rather than reading byte 0; ActionTable, Meter and
   Router fail closed and report invalid offsets from `OnEvent(PreResume)`.
4. **Published state is never mutated in place, except where a structure is
   built for it.** `RcuPtr` generations are immutable once published. The
   exceptions are `ConcurrentExactTable` and `RouteTable`: DPDK designed them
   for one writer changing them in place under lock-free readers, and they
   are wired to the same grace periods (QSBR) as everything else.
5. **Memory is freed only after a grace period.** Every structure hands
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
| flows the packet path creates (NAT-style learning), with stable ids, aliases and a fixed capacity | `flow::WorkerFlowTable` (worker-owned) or `flow::SharedFlowTable` (shared lookup); see [flow-state.md](flow-state.md) | `flow/worker_flow_table.h`, `flow/shared_flow_table.h` | worker-owned / in place, one writer lock |
| flows that need none of that, owned by one worker | `utils::CuckooMap` owned by one worker | `utils/cuckoo_map.h` | worker-owned |
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

- **Writers (D-028):** `kSingle` (the default: one writer at a time, the
  control plane or the worker that owns a partition) or `kShared` (any
  thread; writers serialized by the table's lock; `InsertIfAbsent` for
  race-free flow learning; fixed capacity). Partition when flows can be
  steered or their creation handed to an owner; share when new flows are
  rare (numbers in D-028).

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
- **`Router`:** adds next hops (`NextHopId` → `NextHop`) in a `SlotTable`
  (mode C), so a neighbour change publishes one next-hop object: 63 ns at
  any table size, against 1.2 µs at 1K and 101 µs at 64K next hops when the
  whole next-hop table was rebuilt (P-core, ABBA). Route lookups were
  unchanged or faster (−8% at 1K routes on a P-core). Ordering is enforced:
  - a route can only name an existing next hop;
  - readers fence between the route and next-hop loads;
  - a next hop cannot be removed while any route names it;
  - removing a next hop does not wait. The id stays published and
    unusable ("retiring") until readers pass a grace period, and later
    control calls drop it.
- **Route domains (M6, D-046):** one `Router` owns every domain's FIB and the
  shared next hops. `Router::Create(..., max_domains)` fixes a dense domain
  index (default 1); domains live in a `SlotTable` indexed by `RouteDomainId`,
  so `Resolve(domain, ip)` / `ResolveBatch(domain, ips, hops)` are lock-free,
  need no `Module`, metadata or gates, and miss on an unknown domain.
  `CreateDomain`/`RemoveDomain` are safe while readers run.
  `SetRoute`/`RemoveRoute` are the live in-place update;
  `ReplaceRouteSetAtomic(domain, routes)` builds a replacement FIB beside the
  live one, validates next hops and rte_lpm capacity before publishing, and
  publishes it with one pointer store (a table build, not an in-place update).
- **Used by:** IPLookup, the `Router` module.

- **Next-hop groups** (M15, D-065): `SetNextHopGroup(id, members)` and
  `SetRoute(domain, prefix, NextHopGroupId)`; `Resolve`/`ResolveBatch`/
  `LookupRoute` take a flow hash that picks the member. Transactional when
  enrolled: with `max_groups`, `Enroll` registers `<router>/groups` beside
  next hops and routes, so `SetNextHopGroupOp`, `RemoveNextHopGroupOp` and
  `SetRouteOp(domain, prefix, NextHopGroupId)` commit together with next-hop
  and route changes. A group references its distinct members and a route the
  group it names: the engine refuses a member or group still in use. The
  direct group setters belong to an unenrolled router (`kEnrolled`). The
  `Router` module does not expose groups.
  Neighbor state lives in `NeighborTable` (control side), whose updates are
  next-hop updates; TTL, MTU, ICMP and ARP mechanics are in `l3_packet.h`.

### `ObjectTable<Id, T>` (id → object, mode G)

- **What:** an immutable dense array indexed by a strongly typed, one-based
  id, for when a lookup result is better carried as a compact id than as an
  inline value (next hops, actions, sessions).
- **Id semantics:**
  - ids are stable across generations;
  - erasing leaves a hole, which is never refilled automatically, because
    reusing an id is an ABA hazard that RCU does not solve;
  - who allocates ids decides reuse.

### `SlotTable<Id, T>` (id → object, mode C)

- **Use for:** objects other tables refer to by id (actions, next hops,
  meter policies) when they change one at a time. It replaces
  `ObjectTable`'s rebuild.
- **Reader:** `Lookup(id)`, one acquire load.
- **Writer:** `Publish(id, object)` is one pointer store, and the replaced
  object is retired after a grace period. Removal has two steps:
  - `Retire(id)`: absent on the control side, but still readable;
  - `Unpublish(id)`: empties the slot, run later by the transaction
    engine's removal cascade.
- Code: `core/dataplane/slot_table.h`; D-021.

### `ScopeTable<Version>` (scope id → one immutable policy, mode C)

- **Use for:** a policy that must be seen whole: a session's meter, next
  hop and class, or a VFP-like group's layers. The application decides
  what a scope is; BESS stores one immutable `Version` per `ScopeId` and
  knows nothing else about it. It is a `SlotTable<ScopeId, Version>`, so
  the reader and writer sides below are `SlotTable`'s.
- **Reader:** bind the scope **once** per packet operation --
  `const Version *v = table.Lookup(scope_id)` (one acquire load) -- and use
  `*v` for every covered lookup. Looking the scope up again for each field
  can see two versions (the model test shows it does).
- **Writer:** a transaction on a `ScopeResource<Version>` (key =
  `EncodeKey(ScopeId)`, value = the `Version`) replaces the whole version by
  one pointer store. It is the only built-in resource that can be part of a
  scope-snapshot transaction (`Consistency::kScopeSnapshot`); see
  [architecture.md section 3](architecture.md) for what that does and does
  not promise.
- **A `Version` is immutable and self-contained.** Mutable state (meter
  tokens, counters) lives elsewhere and the version names it by id --
  declare those references like any resource's, and the engine keeps what a
  live version names alive. Build objects a new version names in an earlier
  referential transaction.
- Code: `core/dataplane/scope.h`; D-050.

### Transactions over several tables (G1.2b)

A module exposes its tables as **resources** and registers them with a
`TransactionEngine`. A controller then changes several resources, across
modules, in one call, and packets never see a failed change or a
reference to something missing.

- **Ready-made resources** (no reserve/publish code to write):
  - `SlotResource<Id, T>` over a `SlotTable` (key = id, value = `T`);
  - `ScopeResource<Version>` over a `ScopeTable` (key = `ScopeId`, value =
    the `Version`); the only one that provides the scope-snapshot level;
  - `ExactRuleResource` over a `ConcurrentExactTable` (key = key bytes,
    value = `uint64_t`);
  - `MaskedRuleResource` over a `ConcurrentMaskedTable` (key = mask and
    value bytes, value = `{priority, result}`).

  Each takes a function naming the keys a value refers to (a rule's
  action, an action's meter).
- **Declared dependencies, not ranks:** a resource names, when constructed,
  the resources its values may reference (`SlotResource(name, table,
  references_fn, {"meters"})`). The engine derives the publication order from
  the declarations and refuses undeclared references. Upserts publish
  referents first; erases remove referrers first.
- **A declared reference binds when its resource registers**, so module
  creation order (the desired-state planner's, by name) does not decide the
  graph. Until it binds, a value naming it is refused as an undeclared
  reference -- no key can hold an outgoing reference the ledger does not know
  about. A declared cycle has no publication order: `Apply` refuses until the
  graph changes.
- **Semantics:**
  - all or nothing, with every check and allocation done before anything
    is visible;
  - per-operation results in request order;
  - `expected_generation` for optimistic concurrency;
  - a requested **consistency**: *referential* (the default) or
    *scope-snapshot* (below).

  Referential: packets may see a successful transaction's operations take
  effect one by one, in dependency order, so a packet can see part of it.
  Scope-snapshot: only for resources that provide it (`ScopeResource`);
  each scope switches from its whole old version to its whole new one. A
  scope-snapshot request that names any other resource gets
  `Outcome::kUnsupported` (over RPC, `UNIMPLEMENTED` /
  `UNSUPPORTED_TRANSACTION`) and nothing is applied: it is refused, not
  served as referential (D-050).
- **Only resources that keep erased keys readable** (`DefersErase()`) may
  be referenced. An `rte_hash` rule table erases at once, so it is a root:
  nothing may point at it. Ranks are checked: a referrer must rank
  strictly above what it names.
- **Readers of a rule table registered as a resource drop `kPending`
  hits** (`ExactRuleResource::VisibleHits`). New keys are placed during
  prepare with that value, so a key that cannot be placed rejects the
  transaction instead of failing at publish.
- **Direct commands stay direct** on tables that take part in no
  references; a one-operation transaction costs about 307 ns against 44 ns
  for a direct write (P-core). Tables that do take part in references are
  written only through the engine.
  `ExactMatch` in action mode uses the engine even for `add`, `delete`,
  `clear` and `set_runtime_config`: a command bypassing it would not update
  the rule-to-action reference ledger. Gate mode keeps direct writes.
- **The runtime has one engine**, `runtime().transactions()`. Call
  `Apply()` under the control-plane lock, as module commands run: a
  module's own commands write the same tables.
- **Modules taking part today:** ExactMatch registers its rules as
  `<module>/rules` (key: the fields' bytes in order; value: the gate -- or, in
  action mode, the ActionId of the action the rule names) from `Init()` to
  `DeInit()`, so one transaction can change the rules of
  several ExactMatch instances. Its table grows during prepare when a new
  key does not fit (`ExactRuleResource::Hooks::make_room`); pending keys
  move to the new table with the rest. D-022, D-032.
- **WildcardMatch** registers its rules as `<module>/rules` (key: packed
  mask then packed value; value: `{priority, gate}`) through
  `MaskedRuleResource`. A rule being prepared sits in its tuple naming a
  record that loses to every rule and alone reads as a miss; the table's
  lookup filters it, so the packet path is unchanged. The result 0xFFFF is
  reserved for it. D-024.
- **Router** (opt-in, `Router::Enroll(engine)`): next hops as
  `<router>/next_hops`, routes as `<router>/routes` (each route references
  its next hop). An enrolled router is written only through the engine
  (the direct setters refuse, `kEnrolled`). New routes are placed during
  prepare with the value their addresses already resolve to, so rte_lpm
  capacity is settled before anything is visible. A change costs
  ~0.2-0.6 µs as a transaction against 0.06-0.2 µs direct. D-023. The
  routes resource covers every route domain: the key carries the domain in
  bits 40-63 (the default domain's keys are unchanged), and domains are
  created before `Enroll()` and frozen after it. D-046. The
  `Router` module enrolls on `Init()` and releases on `DeInit()`; its packet
  path resolves a next-hop id from metadata (D-032).
- **ActionTable** registers `<module>/actions` (key: `EncodeKey(ActionId)`,
  value: `{meter id, next hop id}`) and declares both the meter and the
  next-hop resources: an action's value references them, so the engine orders
  the meter and the next hop before the action, refuses an action naming a
  missing one, and refuses the removal of one still named. D-032.
- **Meter** registers `<module>/meters` (key: `EncodeKey(MeterId)`, value: a
  profile specification) over a `MeterSetBuilder`: a transaction edits a
  private clone of the desired state, so unchanged meters keep their token
  state and a rejected transaction leaves the live builder alone. Erases are
  the two-step removal SlotTable defines -- readable until the removal
  cascade reaches them, unpublishable until then. D-032.
  An erased meter stays in generations published by other upserts of the
  same transaction until its removal stage; an old action may still name it.
- **A module must release its resources before destroying their tables.**
  `Unregister` refuses (with the reason) while keys that may reference others
  remain (a resource that references nothing may go with its keys), keys are
  referenced, a registered resource depends on it, or removal steps are
  pending; with workers paused, one call advances the cascade. A module that
  may be referenced *or* hold references -- the four in the session slice --
  uses `ReleaseForTeardown` from `DeInit()` instead: declarations naming a
  released resource are left declared but unbound (a replacement module with
  the same name binds them again), live keys may leave, and dangling
  references are logged. Teardown then does not depend on the order modules
  are destroyed in, which the planner decides by name.
  Registration and teardown rebuild incoming counts from surviving
  resources' committed references (`VisitReferences`), so a same-name
  replacement binds without losing surviving referrers or inheriting
  references from released ones.
- **`kBusy`:** when readers are slow to quiesce and reclamation is behind, a
  transaction is refused retriably instead of waiting.
- **Declare each operation's footprint** in `Reserve()` (`Footprint{retires,
  removals, callbacks}`); the adapters do. Exceeding it is fatal: the
  engine reserved exactly that, so publication never allocates.
- **Cost:** a session of two meters, two actions and two rules, created
  and then removed, takes about 4.4 µs in process (226K sessions/s on a
  P-core, 216K with an idle reader online; 167K and 152K on an E-core).
  With 1-4 busy readers doing chain lookups, the writer sustains 100K
  sessions/s at a 6-11% cost to the readers, and reaches 194-241K sessions/s
  flat out on P-cores (117-168K on E-cores) (`BM_LookupsUnderTransactions`;
  D-021 amendment 4).
- Code: `core/dataplane/{resource.h, transaction_engine.{h,cc},
  slot_resource.h}`, `core/classifier/exact_rule_resource.h`,
  `core/classifier/masked_rule_resource.h`, `core/modules/exact_match.cc`,
  `core/modules/wildcard_match.cc`, `core/modules/action_table.cc`,
  `core/modules/meter.cc`, `core/modules/router.cc`, `core/route/router.cc`;
  D-020 to D-024, D-032.

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
- **Use:** only as state owned and written by one worker. It is not safe for
  a command thread, or a second worker, to write while a worker reads. It
  grows itself when an insert fails, so it is not a fixed-capacity table;
  flow state that needs a bounded size, a stable id or aliases belongs in
  `flow::WorkerFlowTable` (flow-state.md).

### `l2::Fdb` and `l2::MacTable` (L2 forwarding database, worker-owned)

- **What:** `core/l2/fdb.h` (experimental, M14, D-064): exact
  (bridge domain, MAC) → `InterfaceId` lookup, static entries, a learning
  helper (refresh, move, refuse multicast and the invalid interface), aging
  on the generic `ExpiryWheel`, flood groups per domain and a VLAN → domain
  map. No `Module`, gate or runtime dependency; the owner maps interfaces to
  gates (Bridge: gate g is interface g + 1).
- **Table:** `MacTable`, a two-choice cuckoo table whose 64-byte bucket holds
  four contiguous keys (one vector compare), their values and flags; the aging
  timer sits in a cold array beside it. Fixed capacity, sized for 50% load;
  tables of 2 MiB or more ask for transparent huge pages.
- **Use:** one worker owns it (learning writes it on the packet path). It is
  not mode C by default: the table is a type parameter (`BasicFdb<Storage>`,
  D-073), and `PackedMacTable<Cold, SingleWriter|MultiWriter>` gives one
  bridge domain lock-free readers on every worker (below).
- **Cost** (D-064): 3.4 ns a hot lookup, 4.1–5.3 ns uniform up to 64K
  entries; 0.6–2.5 ns behind `l2_table` and 1.1–3× ahead of the
  `unordered_map` Bridge used before M14; at 1M entries 1.5–3.2× behind
  `l2_table`.

### `l2::PackedMacTable<Cold, Sync>` (one-word MAC table, shared readers)

- **What:** `core/l2/packed_mac_table.h` (D-073): MAC (48 bits), value (14),
  a flag and an occupied bit in one 64-bit slot; MacTable's hashing, 4-way
  32-byte buckets at 50% load and breadth-first move search; a cold word per
  slot that only the writer touches. One bridge domain per table.
- **Sync:** `OwnerWrites` (plain stores), `SingleWriter` (the caller
  serialises writers; readers on any worker), `MultiWriter` (the table's
  spinlock, `Lock`/`TryLock`).
- **Semantics for shared readers:** every slot write is one release store;
  a move writes its destination before clearing its source, and a move
  path is bracketed by an odd sequence number that a reader re-checks only
  on a miss, so a key present throughout is never missed and a hit costs no
  extra. The re-check is bounded (`kMaxRetries`, 64): a writer preempted
  inside a move path cannot stall readers, at the price of a rare miss. Proven by deterministic tests at each point inside a move
  (`packed_mac_table_test.cc`), each of which fails if that ordering is
  removed.
- **Used by:** L2Forward (`SingleWriter`); `BasicFdb<PackedMacTable<...>>`
  for a one-domain FDB shared by workers.
- **Cost:** COST

### Member selection (`dataplane/member_select.h`, M16)

- **What:** hash → member index: `RangeSelect` (no table), `WeightedSelector`
  (alias table), `MaglevSelector` (consistent), `RendezvousSelector` (exact
  minimal disruption, O(n): small groups), `RoundRobinCursor` (mutable, one
  owner), `AnySelector` (run-time choice, one dispatch per batch). Built on
  the control side, immutable, published like any object (D-066).
- **Used by:** HashLB and the Router's next-hop groups (`RangeSelect`).

### `ExtractPlan` (packet → key)

- **What:** compiles a key layout (packet offsets, metadata attributes, masks)
  once per generation and extracts a batch of keys with bounds checks. Short
  or multi-segment packets fail extraction, and the module sends them to its
  default gate instead of reading past the data.

## 5. Built-in modules today

| module | table | how changes apply | notes |
|---|---|---|---|
| ExactMatch | `ConcurrentExactTable` | C: add/delete/clear in place; default gate and restore by G; transactions through resource `<module>/rules` | 0.3 µs per add at any size; D-022 |
| IPLookup | `RouteTable` (`rte_lpm`) | C | |
| WildcardMatch | `ConcurrentMaskedTable` (one `ConcurrentExactTable` per mask) | C: add/delete in place; a new or vanished mask republishes only the tuple list; transactions through resource `<module>/rules` | D-014, D-024 |
| L2Forward | `PackedMacTable<SingleWriter>` (one-word slots, 4-way buckets) | C: single-writer, lock-free readers; whole-word slot stores, moves destination-first under a sequence number readers re-check on a miss, no grace period. Multi-entry `add`/`populate` are all-or-nothing per command (validation plus rollback), but not dataplane-atomic: packets see entries one by one | D-017, D-073 |
| ACL | `std::vector` of rules, linear scan | G: `add` copies, appends and publishes (all or nothing) | D-017; `rte_acl` (G-only) is the candidate for large rule sets |
| HashLB | configuration only (`ExactMatchTable` for field layout) | G: one `RcuPtr<Config>`, read once per batch | D-017 |
| URLFilter | `Trie` per host | Pause | legacy: cleartext HTTP only; a modern SNI classifier is recorded in MODERNIZATION §31.6 |
| BPF | compiled filters | Pause | deferred: G together with the `rte_bpf` decision (MODERNIZATION §31.6) |
| NAT | `nat::Nat`: bindings on `WorkerFlowTable` (internal endpoint key, external alias), `PortPool` bitmaps, expiry wheel | worker-owned (the packet path creates mappings) | one worker; D-068 |
| DRR | `CuckooMap` of flows | worker-owned: upstream workers hand packets over an MP/SC ingress ring; the task's worker owns the flow map and queues; commands are atomics | D-019 |
| Bridge | `l2::Fdb` (`MacTable` + `ExpiryWheel`) | worker-owned (the packet path learns); commands are THREAD_UNSAFE | one worker; D-064 |

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
while packets flow, and nothing needs privileges.

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
| L2Forward's `PackedMacTable` | none: every batch hashes, prefetches both buckets, then probes (as MacTable) | - |
| NAT's binding table (`WorkerFlowTable`, D-068) | capacity, at module Init | `Nat::CapacityFor(addresses)`: the mappings the addresses' ports can serve, at most 1M; `FindBatch` always prefetches |
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
- To let the table join transactions with other modules' tables, wrap it
  in a ready-made resource (`ExactRuleResource`, `SlotResource`), register
  it with `runtime().transactions()` in `Init()` and unregister it in
  `DeInit()`, and make the packet path treat `kPending` as a miss (ExactMatch:
  a vectorized mask pass, not measurable; a compare inside the gate loop
  cost 4% on an E-core; D-022).
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
