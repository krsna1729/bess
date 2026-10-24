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
2. **Workers only read shared tables.** Writes come from the control thread
   (the v2 control API: resources and actions). State that the packet path
   itself writes, such as learned flows or counters, is worker-owned instead
   (section 3).
3. **Resolve metadata offsets per batch, and fail closed.**
   `Module::Init()` registers metadata attributes; it does not assign
   graph-relative offsets. `ComputeMetadataOffsets()` assigns offsets from the
   connected graph.
   `PauseAll()` stops workers but does not compute the layout. `ResumeAll()`
   invokes `SetupMetadata` before `resume_all_workers()` but does not pause
   workers itself. In the v2 path a transaction pauses only where a converged
   resource requires it (`requires_paused_workers`, control_plane.cc:1696) and
   that pause is scoped to the operation, so nothing resumes behind a caller's
   back. Keep workers quiescent for
   layout computation. Check `bess::metadata::IsValidOffset()` and drop or
   take a default path rather than reading byte 0; Meter, Router and a
   classifier's result placement fail closed and report invalid offsets from
   `OnEvent(PreResume)`.
   A module whose decision source is configuration rather than metadata has no
   offset to check: the Router's `ROUTE_MODE_DESTINATION` reads the packet's
   own destination instead, which is exactly why the mode is configuration and
   not an inference about the graph (D-155).
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
| masked 5-tuple **with port ranges**, both families, order = policy order | `FirewallTable` | `classifier/firewall_table.h` | rebuild + swap (mode G) |
| masked match with priorities and inclusive per-field **ranges**, changed one rule at a time | `ConcurrentMaskedTable` (WildcardMatch's) | `classifier/concurrent_masked.h` | in place (mode C), D-186 |
| exact match with a compile-time key type (a struct you own) | `ExactTable<Key, Result, Backend>` | `classifier/typed_exact.h` | rebuild + swap (G) |
| IPv4 longest-prefix match | `RouteTable<Value>`; `Router` for routes + next hops + groups | `route/route_table.h`, `route/router.h` | in place (C) |
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
| ExactMatch, one rule | 426 µs at 1K rules, 47 ms at 100K, 645 ms at 1M | 0.3 µs at any size |
| `RouteTable` (`rte_lpm`) | 9.4 ms at 1K routes, 341 ms at 64K; a 512K build takes ~21 s | 0.5-33 µs |
| WildcardMatch (masked, 8 tuples) | 7 µs at 8 rules, 500 µs at 256 rules | (G today; see section 5) |
| FirewallTable (masked + port ranges, both families) | ~50 µs at 1K rules, ~1 ms at 10K | (G) |

A whole-table swap is reserved for cases with no incremental alternative:
`Clear()` on a route table, and structures such as `rte_acl` that can only be
compiled whole. Desired state is not one: `ApplyPipeline` diffs a resource into
per-key upserts and erases, so it goes through the incremental mode.

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
  `LookupBatchPrehashed(keys, stride, sigs, values, n)` is the same lookup
  for a caller that has already hashed the keys with `detail::CrcFixed`
  (ExactMatch's fused kernel, D-121).
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
  and a lookup masks the batch once per tuple (a fixed-width kernel for every
  key width, D-124), looks it up, and merges by priority.
  - Rules must be canonical (`value & ~mask == 0`); anything else is
    rejected at build.
  - Equal priorities resolve to the later rule.
- **Updates:** immutable once built; published in a generation through
  `RcuPtr`.
- **Used by:** WildcardMatch.

### `FirewallTable` (masked 5-tuple with port ranges, both families, mode G)

`classifier/firewall_table.h`. A policy of ordered rules - source and
destination prefix, protocol, source and destination port range, an action
word - where **the first rule that matches wins**, as a firewall's list does.
IPv4 and IPv6 rules live in one table: the family is part of a group's
identity, so a v4 rule never sees a v6 packet, and a rule with no prefix at all
sees both.

The index is tuple space over the **masked fields only**: one group per
(family, protocol, source prefix length, destination prefix length), each
holding one array of (masked key -> rule) entries sorted by key. **The port
ranges are not in the key**: they are checked after the group's key hits, so a
range costs no group and no mask. Before D-186 `WildcardMatch` could not do
that - it had to write a range as 2n-2 prefixes per field and pay for each one
in its tuple budget (D-102); it now takes an inclusive range per field, which
costs no tuple either (D-186 compares the two tables on one policy).

Measured (`classifier_firewall_table_bench`, release, `-march=x86-64-v3`, 32
keys per batch, pinned to one core; see [benchmarking.md](benchmarking.md) for
the profile and the caveat on absolutes): 114 M keys/s for a 10-rule policy
whose keys hit the first rule, 34 M/s for a 1,000-rule policy with the hits
spread over its rules, 26 M/s for the same policy over IPv6, against 0.47 M/s
for the scalar scan of that list. A 10-rule policy that matches its first rule
is *cheaper to scan* (117 M/s) than to index, because the index pays a group
scan and a binary search per key: a handful of rules wants the scan itself (or
`RuntimeMaskedBackend`), and the index's advantage grows to 72x at 1,000 rules.

That scan is a class: `FirewallScan`, in the same header. It holds the rules in
policy order and answers `Lookup`/`LookupBatch` with the same `Match` shape and
the **same predicate** - a lookup packs the key to the rule's own prefix lengths
and calls the `Matches` the index calls per group - so the two structures cannot
disagree about what a rule means, and they refuse the same policies with the
same message. It pays one packing per rule rather than one per group, which is
exactly the trade above: it is the right structure for a handful of rules, and
it is the one to use when a caller holds **one policy per session** (the UPF's
PDR matcher), where a few rules and no groups is both the smallest memory and
the fastest answer. The index is what a policy of hundreds of rules wants.

One change rebuilds the whole table (mode G): about 50 µs at 1K rules and about
1 ms at 10,000. A per-session table with 100K+ rules therefore wants sharding
(one table per direction, or per UE group) or an incremental path that does not
exist yet.

**The module's rules are keyed (D-179).** `<name>/rules` is a map: key
`FirewallRuleKey{id}`, value `FirewallRuleValue{priority, rule}`, ordered by
priority (highest first), then id (lowest first). An id is stable, so a
controller changes or removes one rule by name. A transaction builds the table
once however many rules it changes (`Resource::Prepare`) and swaps it in with
one RCU exchange: through the module, one rule changed costs 0.17 ms at 1K
rules, 2.1 ms at 10K and 13 ms at 50K, and 1,000 changed together cost 1.2,
3.7 and 15.6 ms (`modules_firewall_update_bench`).

Packet path: the module (`modules/firewall.h`) extracts the key - IPv4 by
offset, IPv6 through its extension chain (bounded at four headers) to the
transport header - and a packet the table cannot judge (a non-IP frame, a
truncated header) is dropped rather than passed by a catch-all rule. A
fragment whose ports cannot be read is looked up with both ports 0, so only a
rule whose range starts at 0 can match it. Masking is the `MaskedAddrPair16x2`
kernel (`classifier/firewall_kernels.h`); an IPv4 key is one 64-bit register, so its whole
masking step is two 32-bit loads, a shift, an OR and an AND.

**Actions.** A rule carries an `action_id` (D-141) beside its `drop` flag: the
module writes it into the packet's `action_id` metadata, which a module
downstream reads to select an action, so a matched rule names an outcome rather
than only "pass" or "drop". 0 means no action, and it is written on every pass
so a stale id cannot reach a later lookup; a rule that both drops and names an
action is refused (the packet never reaches a module that would resolve it).
Two more shapes are refused rather than guessed at: a rule naming both families
(one rule is one family, not a disjunction), and a port range the wrong way
round. A *buffering* PDR is not a rule shape: the module forwards on the gate
it arrived on, and the module the action selects names the next hop -
including a packet store's input gate - so `firewall -> packet_store` carries
it, with the store's own `flows` entry holding the duration.

**Sharding is the convention, not a workaround.** The key is a concrete struct,
so a holder cannot add a dimension to it. A field that *partitions* the policy -
a UPF's route domain, one table per domain per direction - belongs in the table
set; a field that *varies within* a policy (a QFI, a tunnel) belongs in the key.
A rebuild is ~50 µs at 1K rules and ~1 ms at 10,000, so a per-session policy of
65k sessions wants one table per direction, per domain or per interface rather
than one table for everything. `Stats.groups` is the invariant to test from
outside: **a range-only change must not move it** - that is the property that
justifies the whole design.

**Three field traps, one line each.** `""` means any address *of the rule's
family*, while `0.0.0.0/0` pins IPv4 - two ways to say "any" with different
family effects. The family comes from the prefixes, and a rule naming both is
refused. An unset port bound is 65535, not 0, because the proto field is
`optional`.

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
  `CreateDomain`/`RemoveDomain` are safe while readers run; a domain is created
  when it is first needed -- explicitly, or by the first route a transaction
  writes into it (D-158) -- so `max_domains` bounds the ids without allocating
  every FIB.
  `SetRoute`/`RemoveRoute` are the live in-place update;
  `ReplaceRouteSetAtomic(domain, routes)` builds a replacement FIB beside the
  live one, validates next hops and rte_lpm capacity before publishing, and
  publishes it with one pointer store (a table build, not an in-place update).
  The guide to the library and to the two ways a graph routes is
  `docs/routing.md`.
- **Used by:** the `Router` module.

- **Next-hop groups** (M15, D-065, D-184): `SetNextHopGroup(id, members,
  standby)` and `SetRoute(domain, prefix, NextHopGroupId)`; `Resolve`/
  `ResolveBatch`/`LookupRoute` take a flow hash that picks the member. A
  group has active members and optional standby members, 1..64 next hops in
  all, a repeated one a weight, none in both lists. Each next hop has an
  availability input (`SetNextHopDown`, or `<router>/next_hop_state` when
  enrolled): a group selects among its available actives, else its available
  standbys, else every active (fail open, so the neighbor state decides). The
  eligible set is part of the immutable group object, so a change of a next
  hop's availability publishes a new object for every group holding it; with
  no availability entries selection is what it was before path health, and a
  member that goes down moves only its own flows. Transactional when
  enrolled: `Enroll` registers `<router>/next_hop_state` and
  `<router>/groups` beside next hops, neighbor bindings and routes --
  always, because a class must register every table it declares (D-156);
  without `max_groups` the groups resource exists, holds nothing, and every
  group id is invalid.
  `SetNextHopGroupOp`, `RemoveNextHopGroupOp`, `SetNextHopStateOp`,
  `RemoveNextHopStateOp` and `SetRouteOp(domain, prefix, NextHopGroupId)`
  commit together with next-hop and route changes. A group references its
  distinct members, a state entry its next hop (a bound one's binding in
  `<router>/neighbors`, D-183), and a route the group it names: the engine
  refuses a member, a next hop with state, or a group still in use. Every
  member pick, IPv4 or IPv6, reads the eligible set. The direct setters
  belong to an unenrolled router (`kEnrolled`).
  The `Router` module exposes groups as `<router>/groups` with `max_groups`
  (D-156): `RouterGroupIdKey -> RouterGroupValue` holds `member_ids` (the
  actives) and `standby_member_ids`, each in selection order; it exposes
  availability as `<router>/next_hop_state` (`RouterNextHopIdKey ->
  RouterNextHopState`, D-184). A route's value names a next hop or a group,
  exactly one (`RouterRouteValue`, D-156), and a group route picks its member
  with `route::FlowHash` over the frame's addresses and ports, the one
  function the appliance hashes with too. Neighbor state lives in
  `NeighborTable` (control side), whose updates are next-hop updates; TTL,
  MTU, ICMP and ARP mechanics are in `l3_packet.h`.

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
    value bytes, then the rule's ranges; value = `{priority, result}`).

  Each takes a function naming the keys a value refers to (a rule's
  meter and next hop).
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
  An `ExactMatch` whose result fields name resources writes its rules only
  through the engine (`<name>/rules`): a write bypassing it would not update the
  rule-to-referent ledger. A gate-only table keeps direct writes.
- **The runtime has one engine**, `runtime().transactions()`. Call
  `Apply()` under the control-plane lock: the control plane, and a module's own
  resource writes (including an `edits_resources` action such as L2Forward's
  `populate`), touch the same tables.
- **Modules taking part today:** ExactMatch registers its rules as
  `<module>/rules` (key: the fields' bytes in order; value: the output gate and the rule's result record id,
  whose bytes `result_fields` places on a hit, D-172) from `Init()` to
  `DeInit()`, so one transaction can change the rules of
  several ExactMatch instances. Its table grows during prepare when a new
  key does not fit (`ExactRuleResource::Hooks::make_room`); pending keys
  move to the new table with the rest. D-022, D-032.
- **WildcardMatch** registers its rules as `<module>/rules` (key: packed
  mask then packed value; value: `{priority, gate, result}`, where `result` is the
  rule's record bytes and `result_fields` places them on a hit, D-172) through
  `MaskedRuleResource`. A rule being prepared sits in its tuple naming a
  record that loses to every rule and alone reads as a miss; the table's
  lookup filters it, so the packet path is unchanged. The result 0xFFFF is
  reserved for it. D-024.
- **Firewall** registers its rules as `<module>/rules` (key: the rule id;
  value: `{priority, rule}`) and rebuilds its table once per transaction in
  `Prepare()`; the first of its operations to publish swaps the policy in
  whole, so the workers are never paused. D-147, D-179.
- **Router** (opt-in, `Router::Enroll(engine)`): next hops as
  `<router>/next_hops`, their availability as `<router>/next_hop_state`
  (each entry references its next hop, D-184), groups as `<router>/groups`
  (active and standby member lists, D-156, D-184), routes as
  `<router>/routes` (each route references the next hop or the group it
  names). A state change republishes every group holding the next hop, in
  the same transaction. An enrolled router is written only through the
  engine (the direct setters refuse, `kEnrolled`). New routes are placed during
  prepare with the value their addresses already resolve to, so rte_lpm
  capacity is settled before anything is visible. A change costs
  ~0.2-0.6 µs as a transaction against 0.06-0.2 µs direct. D-023. The
  routes resource covers every route domain: the key carries the domain in
  bits 40-63 (the default domain's keys are unchanged), and domains are
  created by the first route write into them, so `max_domains` bounds the ids
  while a router pays only for the domains that hold routes. D-158. The
  `Router` module enrolls on `Init()` and releases on `DeInit()`; its packet
  path resolves a next-hop id from metadata, or -- in `ROUTE_MODE_DESTINATION`
  -- runs the library's forwarding chain (`route/forward.h`: the checked parse,
  the destination looked up in the packet's route domain, the neighbor check,
  the TTL and the addresses; D-157), and either way writes a resolved hop's
  addresses into the frame's Ethernet header when it names both (D-032, D-153,
  D-155).
- **A classifier rule's result fields are the referrers.** A `ResultField` may
  name a resource (`mt/meters`, `rt/next_hops`) and how its bytes name an entry
  (`RAW` or `BE32`); the rule resource declares those names, so the engine
  orders a rule after the entries it names, refuses a rule naming a missing
  one, and refuses the removal of one still named (D-172, D-174). A field that
  carries several `BE32` ids (a two-level meter) names each of them
  (`reference_count`).
- **Meter** registers `<module>/meters` (key: `EncodeKey(MeterId)`, value: a
  profile specification) over a `MeterSetBuilder`: a transaction edits a
  private clone of the desired state, so unchanged meters keep their token
  state and a rejected transaction leaves the live builder alone. Erases are
  the two-step removal SlotTable defines -- readable until the removal
  cascade reaches them, unpublishable until then. D-032.
  An erased meter stays in generations published by other upserts of the
  same transaction until its removal stage; an old rule may still name it.
- **Operational state:** the Meter module's `get_status` action reads the
  published generation without pausing workers (it is `THREAD_SAFE`). For each
  configured meter it returns the profile, the sharing mode, the TSC of the
  last update, and the committed and excess/peak buckets with their current
  tokens and limits. It takes no new lock and adds no write, atomic or
  reference count to the packet path: a shared meter is read under the same
  spinlock its check takes, so the pair of buckets is a coherent snapshot.
  The reading is per meter state, never a module-wide total; a
  worker-exclusive meter's tokens belong to its owner and are only coherent
  when that worker is not running the meter (the core
  `MeterState::Inspect()` contract).
- **A module must release its resources before destroying their tables.**
  `Unregister` refuses (with the reason) while keys that may reference others
  remain (a resource that references nothing may go with its keys), keys are
  referenced, a registered resource depends on it, or removal steps are
  pending; with workers paused, one call advances the cascade. A module that
  may be referenced *or* hold references -- the three in the session slice --
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
- **A resource that rebuilds builds once per transaction.** `Reserve()`
  validates and records each operation; `Prepare()`, called once per touched
  resource after every operation was reserved and checked, builds the one
  replacement they make together, and may fail (the transaction is refused
  against that resource's last operation). Every operation's staged work then
  publishes the same replacement, the first one swapping it in. A resource
  that builds per operation would rebuild k times for k operations, and
  per-operation replacements would publish in the engine's order (upserts
  before erases), not the request's. D-179.
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
  `core/modules/wildcard_match.cc`, `core/modules/meter.cc`,
  `core/modules/router.cc`, `core/route/router.cc`;
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

  `MeterState::Inspect()` reads a state without changing that: a shared meter
  is read under its check lock, while a worker-exclusive meter has no lock by
  design and its owner must be quiescent for the read. The snapshot reports
  the meter's `sharing` mode, its profile, the last-update TSC and both token
  buckets; it is a reading of one state, never a sum over workers. See
  `meter/meter.h`.

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
  a flag and an occupied bit in one 64-bit slot; a secretly seeded CRC hash,
  4-way 32-byte buckets at 50% load and breadth-first move search; a cold word per
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
- **Used by:** L2Forward (`SingleWriter`); the Bridge's FDB
  (`BasicFdb<PackedMacTable<ExpiryHandle>>`, owned); a one-domain FDB shared
  by workers (`SingleWriter`/`MultiWriter`).
- **Cost** (D-073): batch-32 lookups 15-43% faster than l2_table at 1K-1M
  (hits and misses), scalar misses 10-15% faster, scalar hits 0.3-0.4 ns
  slower; under the FDB, 20-75% faster than MacTable (1M misses 7.7 ns
  against 24.7).

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
- **Used by:** ExactMatch and WildcardMatch (their keys), and HashLB's custom
  tuple (its key, hashed rather than matched). HashLB has no default gate, so
  a packet it cannot extract a key for is dropped.

## 5. Built-in modules today

| module | table | how changes apply | notes |
|---|---|---|---|
| ExactMatch | `ConcurrentExactTable` | C: add/delete/clear in place; default gate and restore by G; transactions through resource `<module>/rules` | 0.3 µs per add at any size; D-022 |
| Router | `RouteTable` (`rte_lpm`) | C: add/delete in place; transactions through the route, next-hop and group resources | routes, next hops and groups as resources (D-023, D-155, D-156); the route key carries its domain |
| WildcardMatch | `ConcurrentMaskedTable` (one `ConcurrentExactTable` per mask) | C: add/delete in place; a new or vanished mask republishes only the tuple list; transactions through resource `<module>/rules` | D-014, D-024; per-field ranges cost no tuple (D-186) |
| L2Forward | `PackedMacTable<SingleWriter>` (one-word slots, 4-way buckets) | C: single-writer, lock-free readers; whole-word slot stores, moves destination-first under a sequence number readers re-check on a miss, no grace period. Multi-entry `add`/`populate` are all-or-nothing per command (validation plus rollback), but not dataplane-atomic: packets see entries one by one | D-017, D-073 |
| ACL | `std::vector` of rules, linear scan | G: `add` copies, appends and publishes (all or nothing) | D-017; `rte_acl` (G-only) is the candidate for large rule sets |
| HashLB | configuration only (one `classifier::ExtractPlan` for the custom tuple, rebuilt at resume) | G: one `RcuPtr<Config>`, read once per batch | D-017 |
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
  `rte_lpm` in place, and `rte_fib` rejected (amended by D-132: `rte_fib` is a
  selectable backend, `rte_lpm` the default);
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
| NAT's binding table (`WorkerFlowTable` owned, `SharedFlowTable` shared; D-068, D-078, D-079) | `capacity` and `max_capacity`, at module Init | 65,536 or what the addresses' ports serve if less; grows by doubling to `max_capacity` (owned: opt-in; shared: by default, up to what the addresses serve, at most 1M); `FindBatch` always prefetches |
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
- Write only from the control thread: a resource's apply, or an action that
  writes the resource it owns and says `edits_resources` (which advances the
  resource generation).
- Mark an action `THREAD_SAFE` only if it changes state through G, C or W (a
  table a worker may keep reading); otherwise it is refused while a worker runs
  and the client pauses first.
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
| `rte_fib` backend (correctness, D-130/D-132) | `route_route_test` (`RteFibTest.*`, every build) and the `/Fib` instances of the route tests; `route_bench`, `fib_bench` and `live_perf M.router/fib` for cost |
| LPM lookup and update cost | `route_bench` |
| the Router's destination path at scale (route count × destination spread), LPM and FIB | `live_perf --tests M.router,M.router/s256,M.router/s64k,M.router/r2560s64k,M.router/r2560,M.router/r10240,M.router/r65536,M.router/r2560s64k-fib` |
| meters, counters, RCU | `meter_bench`, `stats_bench`, `rcu_bench` |

### Classifier result placement: live comparison

`tests/perf/live_perf.cc` contains `M.place/*` arms for direct EM/WM result
placement and EM→EM / WM→EM composition. The first table has 1,024 or 65,536
rules. Uniform records contain 1–8 four-byte fields; `mixed8` and `mixed16`
use heterogeneous widths. The second table holds 100%, 50%, 30%, 20%, 10%,
or 5% as many entries (mixed-width arms use 100%, 50%, and 20%).

RecordArena's record chunks and packet-path chunk-pointer array use the
framework DPDK heap. `bess::utils::DpdkAllocate` follows the initialized EAL:
hugepage-backed when enabled, normal-page-backed under `--no-huge`, using the
framework memory-node policy. Allocation and exhaustion handling remain on
the control path; the arena adds no separate hugepage detection or fallback.
These allocations consume the shared EAL heap budget, including its default
512 MB normal-page budget; provision that budget for result records too.

Hits use connected gate 0 and misses drop. At ratio R, both arms use the same
rule→action→result mapping: rule i produces record i modulo
max(1, floor(N×R/100)). Each direct arm therefore has its own `/rR` case;
the two-stage arm stores the same record behind that action ID.

After timing, the runner pauses workers and checks every required stage's
forwarded count. It then installs a verifier keyed by the original packet
source and every result byte, resumes traffic briefly, and checks that none
of that traffic was rejected. Eight-result-field cases use two verifier
tables to respect ExactMatch's eight-key-field limit. Verifiers are absent
from the measured graph. Failed path or byte checks discard the sample.

Use the same corrected runner for both daemon builds, pinned to an isolated
core with its SMT sibling offline. Save raw rounds with `--save`; compare
cycles and instructions per packet as well as elapsed time, and retain the
spread. Alternate build order (`--rounds 2` runs A/B then B/A). WM uses one
all-ones tuple in these arms: results do not establish the cost of a
many-tuple wildcard policy. Turbo-off does not guarantee a fixed clock on a
laptop, so record the platform power profile too.

Keep the control CPU inside the benchmark partition too: for worker CPU 4
and control CPU 0, use wrapper `--cpu 0,4 --isolate` and runner
`--cpu 4 --control-cpu 0`. Check the daemon's placement line reports control
CPU 0 and worker CPU 4. A runner flag cannot move control threads onto a CPU
excluded by the cgroup; a CPU-4-only partition reports no reserved control
CPU and includes control-thread contention.

The runner defaults to daemon `-m 0` (no hugepages). To require hugepages, pass
an explicit positive memory cap through the build specification, for example
`--build huge=/path/to/bessd,-m,1024`; do not infer the EAL mode from host free
hugepages. Set `BESS_DPDK_LIB` to the repository-local DPDK library directory
when the daemon needs that search path under `sudo`.

#### Final EM/WM placement comparison

The final implementation uses DPDK-backed `RecordArena` storage, inline
accessors, and the existing bucketed `ResultPlan` kernels. The matrix varies:

- Rule-table size N: 1,024 or 65,536.
- Uniform result size M: 1–8 four-byte fields (4–32 bytes).
- Heterogeneous field mix: `mixed8` uses 4+2+1+1-byte fields; `mixed16`
  uses 8+4+2+1+1-byte fields.
- Target action/rule ratio R: 100%, 50%, 30%, 20%, 10%, or 5%; mixed layouts
  use 100%, 50%, and 20%. The action count is floor(N×R/100).
- EAL memory mode: normal pages (`-m 0`) or hugepages (`-m 1024`).

All 432 placement cases passed path and result-byte validation in both
rounds of both EAL modes: 1,728 placement samples. Each round used 1.5 s
measurement and 0.3 s warmup. CPUs 0 and 4 were in the isolated partition,
with SMT siblings 1 and 5 offline; the daemon reported control CPU 0 and
worker CPU 4. Turbo was off, governor/EPP were performance, and the platform
profile was balanced. Packets were 64 bytes, keys four bytes, and traffic
covered the full rule-key range. WM used one all-ones tuple.

Each cell is `cycles/packet / ns/packet`, with that mode's `M.base`
subtracted per round before taking the case median. The ratio tables then
take the median across the listed geometries. These are module-cost deltas,
not total pipeline latency. The EAL-mode comparison changes the whole
daemon's DPDK heap, including hash tables and packet storage; it does not
isolate the effect of RecordArena allocation alone.

##### Normal-page EAL (`-m 0`)

| R | geometries | EM inline | EM→EM | WM inline | WM→EM |
|---:|---:|---:|---:|---:|---:|
| 100% | 20 | 50.55 / 19.48 | 123.29 / 47.49 | 74.66 / 28.74 | 155.54 / 59.90 |
| 50% | 20 | 50.42 / 19.45 | 121.26 / 46.71 | 75.19 / 28.94 | 152.21 / 58.63 |
| 30% | 16 | 51.55 / 19.85 | 116.06 / 44.66 | 74.85 / 28.79 | 143.92 / 55.42 |
| 20% | 20 | 50.37 / 19.42 | 117.55 / 45.26 | 74.56 / 28.70 | 149.39 / 57.56 |
| 10% | 16 | 50.92 / 19.64 | 115.95 / 44.63 | 75.16 / 28.94 | 146.32 / 56.32 |
| 5% | 16 | 51.16 / 19.74 | 112.49 / 43.33 | 74.67 / 28.74 | 142.37 / 54.83 |

Table-size, result-size, and field-mix sensitivity at R20:

| N | layout | EM inline | EM→EM | WM inline | WM→EM |
|---:|---|---:|---:|---:|---:|
| 1,024 | m1 | 42.83 / 16.49 | 102.66 / 39.55 | 61.26 / 23.60 | 126.70 / 48.80 |
| 1,024 | m2 | 42.87 / 16.51 | 103.62 / 39.85 | 61.48 / 23.63 | 125.35 / 48.26 |
| 1,024 | m3 | 48.95 / 18.88 | 110.32 / 42.49 | 72.25 / 27.80 | 132.83 / 51.17 |
| 1,024 | m4 | 43.11 / 16.63 | 103.31 / 39.77 | 58.36 / 22.47 | 123.11 / 47.45 |
| 1,024 | m5 | 51.62 / 19.92 | 111.10 / 42.79 | 74.52 / 28.67 | 134.81 / 51.90 |
| 1,024 | m6 | 50.36 / 19.41 | 113.65 / 43.77 | 74.17 / 28.52 | 136.61 / 52.62 |
| 1,024 | m7 | 50.36 / 19.42 | 110.06 / 42.38 | 74.59 / 28.73 | 134.95 / 51.93 |
| 1,024 | m8 | 47.16 / 18.19 | 104.14 / 40.11 | 64.08 / 24.67 | 128.10 / 49.36 |
| 1,024 | mixed8 | 41.05 / 15.83 | 101.30 / 39.01 | 61.05 / 23.47 | 123.63 / 47.60 |
| 1,024 | mixed16 | 46.23 / 17.83 | 123.78 / 47.71 | 62.07 / 23.89 | 146.37 / 56.40 |
| 65,536 | m1 | 50.25 / 19.35 | 118.25 / 45.52 | 74.96 / 28.83 | 169.34 / 65.21 |
| 65,536 | m2 | 50.38 / 19.42 | 117.72 / 45.32 | 74.28 / 28.62 | 180.78 / 69.57 |
| 65,536 | m3 | 58.98 / 22.72 | 127.99 / 49.28 | 83.71 / 32.21 | 162.67 / 62.58 |
| 65,536 | m4 | 51.48 / 19.82 | 117.38 / 45.20 | 74.87 / 28.84 | 152.40 / 58.72 |
| 65,536 | m5 | 58.16 / 22.43 | 127.53 / 49.10 | 83.36 / 32.12 | 159.59 / 61.50 |
| 65,536 | m6 | 57.79 / 22.29 | 126.21 / 48.64 | 85.12 / 32.77 | 179.06 / 68.95 |
| 65,536 | m7 | 58.22 / 22.42 | 131.02 / 50.41 | 84.60 / 32.59 | 189.23 / 72.76 |
| 65,536 | m8 | 61.43 / 23.73 | 124.53 / 47.93 | 90.26 / 34.71 | 169.19 / 64.94 |
| 65,536 | mixed8 | 50.65 / 19.55 | 123.14 / 47.43 | 82.98 / 31.97 | 165.42 / 63.76 |
| 65,536 | mixed16 | 51.96 / 20.03 | 144.51 / 55.62 | 84.63 / 32.60 | 203.84 / 78.45 |

##### Hugepage EAL (`-m 1024`)

| R | geometries | EM inline | EM→EM | WM inline | WM→EM |
|---:|---:|---:|---:|---:|---:|
| 100% | 20 | 49.37 / 19.03 | 118.88 / 45.77 | 73.51 / 28.32 | 152.86 / 58.88 |
| 50% | 20 | 49.08 / 18.93 | 116.45 / 44.83 | 72.41 / 27.88 | 149.07 / 57.40 |
| 30% | 16 | 48.96 / 18.90 | 112.39 / 43.30 | 72.83 / 28.06 | 141.44 / 54.49 |
| 20% | 20 | 48.87 / 18.86 | 113.21 / 43.58 | 72.98 / 28.09 | 145.24 / 55.93 |
| 10% | 16 | 49.05 / 18.93 | 111.00 / 42.74 | 73.09 / 28.15 | 138.67 / 53.45 |
| 5% | 16 | 49.51 / 19.11 | 109.61 / 42.21 | 73.57 / 28.35 | 139.00 / 53.52 |

Table-size, result-size, and field-mix sensitivity at R20:

| N | layout | EM inline | EM→EM | WM inline | WM→EM |
|---:|---|---:|---:|---:|---:|
| 1,024 | m1 | 42.79 / 16.50 | 103.37 / 39.79 | 61.71 / 23.76 | 125.02 / 48.13 |
| 1,024 | m2 | 43.27 / 16.71 | 106.67 / 41.08 | 62.94 / 24.23 | 127.15 / 48.97 |
| 1,024 | m3 | 50.06 / 19.33 | 110.23 / 42.48 | 74.11 / 28.55 | 134.32 / 51.78 |
| 1,024 | m4 | 44.03 / 17.00 | 102.17 / 39.35 | 61.11 / 23.53 | 124.83 / 48.10 |
| 1,024 | m5 | 48.82 / 18.84 | 109.65 / 42.23 | 73.78 / 28.40 | 136.58 / 52.57 |
| 1,024 | m6 | 50.17 / 19.37 | 109.95 / 42.35 | 74.38 / 28.62 | 134.19 / 51.68 |
| 1,024 | m7 | 49.56 / 19.13 | 110.90 / 42.72 | 74.63 / 28.73 | 136.13 / 52.44 |
| 1,024 | m8 | 45.90 / 17.73 | 104.19 / 40.12 | 61.54 / 23.70 | 128.65 / 49.57 |
| 1,024 | mixed8 | 42.54 / 16.43 | 101.61 / 39.12 | 60.59 / 23.30 | 125.36 / 48.25 |
| 1,024 | mixed16 | 44.58 / 17.21 | 119.61 / 46.11 | 61.26 / 23.60 | 146.20 / 56.37 |
| 65,536 | m1 | 48.25 / 18.61 | 113.96 / 43.89 | 71.01 / 27.34 | 146.23 / 56.31 |
| 65,536 | m2 | 48.61 / 18.73 | 115.41 / 44.44 | 71.46 / 27.48 | 147.24 / 56.71 |
| 65,536 | m3 | 52.94 / 20.43 | 123.67 / 47.64 | 81.05 / 31.22 | 154.58 / 59.51 |
| 65,536 | m4 | 47.64 / 18.40 | 112.45 / 43.27 | 69.00 / 26.57 | 144.28 / 55.55 |
| 65,536 | m5 | 53.34 / 20.58 | 122.77 / 47.27 | 80.63 / 31.05 | 156.45 / 60.26 |
| 65,536 | m6 | 51.18 / 19.75 | 122.72 / 47.23 | 79.91 / 30.76 | 155.72 / 59.96 |
| 65,536 | m7 | 53.75 / 20.71 | 122.82 / 47.29 | 81.19 / 31.28 | 154.37 / 59.48 |
| 65,536 | m8 | 50.64 / 19.54 | 114.28 / 44.03 | 72.19 / 27.77 | 146.46 / 56.36 |
| 65,536 | mixed8 | 51.30 / 19.79 | 117.17 / 45.13 | 76.40 / 29.39 | 168.56 / 64.92 |
| 65,536 | mixed16 | 48.92 / 18.88 | 140.41 / 54.10 | 76.19 / 29.33 | 189.71 / 73.04 |

#### Engineering guidance

Prefer direct final-result placement for packet-path performance. It won all
432 matched classifier/ratio/geometry/memory-mode comparisons, in both
measured rounds: there is no measured crossover. Across the ratio-table
medians, direct EM cost 54.5–59.0% fewer cycles than EM→EM; direct WM cost
47.1–52.0% fewer than WM→EM. This applies to the tested matrix, not larger
tables, values wider than 32 bytes, different key widths, or many-tuple
wildcard policies.

Use a second stage when shared action indirection is required, or when
separately measured memory or update benefits justify its extra lookup.
Memory consumption and update fan-out were not measured here. Do not infer
their tradeoffs from packet-processing timings.

Use the framework allocation policy in either EAL mode; the arena does not
need its own mode switch. These tables do not show that hugepages improve
every case, nor isolate an allocator-only gain. Observable IRQ/softirq
activity remained on CPU 4 despite scheduler isolation. Absolute costs
varied between rounds; these short laptop measurements do not establish a
universal percentage saving.

“Inline” means one classifier produces the final result, not arbitrary-width
bytes embedded in an `rte_hash` scalar. RecordArena holds the records in both
topologies. This is not a benchmark of alternative `rte_hash` value APIs.
Raw rounds are retained locally under
`.scratch/recordarena-evidence/final-allocation/`.

