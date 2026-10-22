# Neighbor resolution

`core/route/neighbor_resolver.h` is the IPv4/IPv6 Ethernet neighbor
resolution library (M15, D-175, D-176): it solicits with ARP or ICMPv6
Neighbor Solicitation, learns replies/advertisements, retries with backoff,
ages resolved neighbors, follows a MAC change, publishes failure and
recovers, and gives the existing neighbor table its events. Packet mechanics
live beside it (`route/l3_packet.h`); adjacency stays in the Router library.
This guide states what existed before it, what the library owns, and how the
dataplane stays consistent while a neighbor changes.

## What existed

**`Vif`** (`core/modules/vif.*`) strips an 802.1Q/802.1ad tag, writes the vif
id and route domain metadata, and demultiplexes by (VLAN, destination MAC).
For ARP it had one hand-written rule: an ARP *request* whose target address is
one of its interfaces' addresses (keyed by VLAN and address) is rewritten in
place into the reply -- opcode, Ethernet addresses and the four ARP addresses
are assigned field by field -- and emitted on that interface's gate. It sent no
request, learned nothing, kept no timer, rejected no stale reply (it had none
to reject) and had no IPv6 ND path; IPv6 frames were demultiplexed like any
other. ARP replies arriving from the network were just demultiplexed too.

**`Router` adjacency** (`core/route/router.h`). A route maps a prefix to a
`NextHopId`; each id names one immutable 16-byte `NextHop`
(`egress InterfaceId`, `NeighborState`, `dst_mac`, `src_mac`) published in a
`SlotTable`. A neighbor change is one `SetNextHop` -- one pointer store and an
RCU retirement -- and never a route change: the FIB keeps naming the same ids.
A packet that resolves to `kIncomplete` or `kUnreachable` is dropped, punted or
queued by the application; the library has no policy for it.

**`NeighborTable`** (`core/route/neighbor_table.h`, control side) is the
adjacency binder: key `(InterfaceId, IPv4)` -> state and MAC plus the set of
next-hop ids bound to it. `Update()` records what resolution learned and
returns every bound next hop's new object; `Publish()` writes them with
`Router::SetNextHop`. It is not thread-safe and runs no protocol. It had no
notion of interface identity beyond the id and no timers.

**`ExpiryWheel`** (`dataplane/expiry_wheel.h`) is the worker-owned timer
wheel: O(1) schedule/cancel, one payload per timer, generation-safe handles,
bounded `Poll` with a budget, no allocation after `Create`. Flow expiry and the
FDB already use it. Neighbor state used no timer.

## The library boundary

`NeighborResolver` is a single-writer state machine over
`(InterfaceId, address)`; the address is IPv4 or IPv6 (D-176):

| event | call | effect |
|---|---|---|
| create | `Bind(hop, key, now)` | binds a next hop through `NeighborTable`, starts resolution if the neighbor is new |
| learn | `OnArp(now, ingress, frame)` / `OnNd(now, ingress, frame)` | a valid reply or advertisement resolves the neighbor and publishes its new MAC |
| refresh | `OnArp` again, or `Poll` when the aging timer fires | re-arms the lifetime; on expiry the neighbor stays `kResolved` and is reconfirmed by probes |
| fail | `Poll` after the last probe (of a first resolution or a reconfirmation) | publishes `kUnreachable` |
| recover | `Poll` after the retry interval | publishes `kIncomplete`, solicits again |
| identity change | `ConfigureInterface` / `RemoveInterface` | cancels timers, invalidates stale replies, republishes |

The state machine is:

```text
            Bind                      reply
   (new) --------> kIncomplete ----------------> kResolved
                       |  ^                       |    ^
             max probes|  | retry        lifetime |    | reply
                       v  |                       v    |
                  kUnreachable <-- max probes -- kResolved, reconfirming
```

`Poll(now, budget)` returns the solicitations to send and the next-hop updates
to publish; `OnArp`/`OnNd` return the updates for a reply. The caller owns packet
buffers, gates and the policy for unresolved traffic. That keeps the library
free of `Module`, gate, worker and protobuf dependencies, as the rest of
`core/route` is.

Interface identity is explicit: `ConfigureInterface({id, mac, addresses})`
(any number of IPv4 and IPv6 addresses, each with its on-link prefix length)
assigns a generation (epoch) per interface. Every entry records the epoch it was created
under, and a reply is accepted only if the arrival interface, the epoch, the
reply's sender and target addresses, its Ethernet source/destination and a
unicast sender MAC all agree with the current identity. A reply in flight when
an address moves is therefore a stale record, not an update to a newer
neighbor. The same key and epoch rules apply to both families, so an IPv4
reply cannot resolve an IPv6 neighbor or vice versa. A reply never creates an entry: only `Bind` does, so unsolicited
frames cannot populate the table. `Unbind` leaves a learned neighbor in the
table (a later bind in the same generation may reuse it), but configuring the
interface with a new MAC or address, and removing it, walk every neighbor key
on that interface -- bound or not -- and invalidate that learning, so a rebind
cannot resurrect a MAC learned under an old interface identity. A rebind also
moves the bindings' captured source MAC, so the republished next hops send
from the interface's current MAC. `Bind` enforces the neighbor-entry bound
itself (a neighbor with aging disabled holds no timer), permits a full table
when the rebind releases its old neighbor's last hop, and rolls the binding
back if the wheel refuses the new timer. `CheckBind(key)` answers whether a
`Bind` of that key would be taken (a known interface, an address of the
interface's families, not one of the interface's own addresses) without
changing anything, so a caller can refuse a binding before it commits to it.

ARP that answers nothing still says where its sender is (RFC 826's merge).
`OnArp` treats an ARP frame from a bound neighbor in one of two ways:

- a reply whose target is one of the interface's addresses, unicast to the
  interface's MAC or broadcast, answers a solicitation: the neighbor resolves
  on the sender's MAC and its reachable lifetime restarts;
- a request (for any target, gratuitous announcements included) or a
  broadcast reply naming another target is a merge: unless the neighbor is
  already resolved on that MAC, the new MAC is published as resolved at once
  and the neighbor is reconfirmed on the next `Poll` (solicited, and failed to
  `kUnreachable` if it does not answer). A unicast reply naming another target
  answers nothing asked and is ignored.

Either way the frame needs an Ethernet source equal to the ARP sender, a
usable unicast sender MAC that is not the interface's own, an Ethernet
destination that is the interface's MAC or broadcast, and the generation the
entry was bound under; a sender no `Bind` named is ignored.

An interface can hold several addresses of a family. A solicitation leaves
from the address whose prefix holds the neighbor, and from the first address
of the neighbor's family when none does; a reply is accepted when its target
is any of the interface's current addresses.

Timing is in caller ticks, matching `ExpiryWheel` and `docs/expiry.md`: the
first probe, the retry interval with exponential backoff capped at a maximum,
the probe count, the reachable lifetime and the unreachable retry. Zero
disables aging or retry respectively. The resolver uses one `ExpiryWheel` of
neighbor capacity -- not a second timer wheel -- with one armed timer per entry
and an arming serial in each payload so a record that outlives its entry
cannot touch a newer one. Re-arming a live timer (a reply refreshing the
lifetime, an identity change) reschedules it in place: the same node, a new
deadline and payload, no capacity spent and no generation advance, so a steady
reply stream cannot exhaust the wheel. A new node is needed only when an entry
has no live timer; the wheel's usable capacity falls when a node's generation
is exhausted (quarantined), so `Schedule` can still refuse. A refusal is
handled, never ignored: `OnArp`/`OnNd` leave the entry, its timer and the
table untouched and ignore the frame, `Bind` rolls back, and an entry whose
`Poll` or `ConfigureInterface` transition cannot be armed is dropped rather
than kept without a timer (its committed state is incomplete/unreachable, so
the hop does not forward). `OnNd` stages its table update and output space and
arms/reschedules the aging timer before committing, exactly as `OnArp` does.

## Publication and the packet path

The resolver never writes a `NextHop` in place. It asks `NeighborTable::Update`
for the new immutable objects and returns them:

- direct users call `Publish(router, updates)` for an unenrolled Router: one
  `Router::SetNextHop` per bound hop, one pointer store each and no FIB write;
- transactional users build `NextHopOps(router, updates)` and apply them in
  one `TransactionEngine::Apply`: all-or-nothing, but referential rather than
  snapshot visibility, so different hops may change one at a time;
- the `Router` module applies resolver-owned next-hop operations through the
  in-process transaction engine, at most 64 per tick in one transaction; the
  rest wait for the next tick (D-180). A bound next-hop id lives in
  `<router>/neighbors`, not `<router>/next_hops`: the next-hop resource
  refuses a write or erase of a bound id and does not enumerate one, so
  desired-state convergence of `<router>/next_hops` leaves bound hops alone.

Aging does not stop forwarding: when a resolved neighbor's reachable lifetime
expires it stays resolved, with its MAC, while the resolver probes it with the
normal backoff, and becomes unreachable only after `max_probes` unanswered
probes (RFC 4861 STALE/PROBE). A reply during reconfirmation keeps it resolved
without republishing. In the `Router` module each `neighbor_*` field is
optional: unset takes the default, and an explicit 0 has one meaning per field
(listed below). ARP replies and Neighbor
Advertisements are queued with the interface id read when they arrive (an
unknown interface or a full queue drops and counts them). ARP requests and
Neighbor Solicitations are answered elsewhere: they go to `punt_neighbor_gate`
(by default the gate `punt_local_gate` names), or are dropped, and are
counted. An ARP request is also copied to the resolver on a ring of its own,
so a busy segment's requests cannot take the room replies need; that copy is
what lets a gratuitous ARP move a bound neighbor.

In each case a packet-path lookup (`Router::Resolve` /
`LookupNextHop`) obtains one whole `NextHop` pointer through the `SlotTable`'s
acquire load. Readers see the old object or the new one, never a half-written
MAC, egress or state; the replaced object is retired after a grace period. The
`ConcurrentPublishAndLookupNeverSeeAHalfUpdate` and
`ATransactionPublishesAllAdjacencyOrNone` tests pin both paths.

Allocation failure is staged away from the dataplane. `ConfigureInterface` and
`RemoveInterface` build every key's update and reserve the output before
committing the interface identity or any neighbor change; `Poll` collects the
due wheel records, plans every transition (including its next-hop update
vector) and reserves the output before committing anything, and if a plan's
allocation is refused it re-arms every due entry at the deadline its record
carried before rethrowing (the wheel had already spent those nodes). `OnArp`/`OnNd`
stage their update and output space and commit the resolved state only once
the aging timer is in place, so a timer refusal leaves the entry and its timer
as they were. A refused call therefore leaves the neighbor table, the
published adjacency and the timers exactly as they were, and the same call
retried converges.
`NeighborResolverFaultTest` fails every allocation point of Poll, Configure and
Remove in turn and checks that invariant, and repeats the ND paths so the
staging is not IPv4-only.

The resolver itself is single-threaded (one control-side writer). ARP and ND
frames arriving on workers must be handed to that writer; the packet path
never calls into the resolver. This is the same ownership split as every other control-side
table in BESS and is what lets the state machine allocate without locks.

## Router module configuration and handoff

`RouterArg.interfaces` lists the router's own interfaces, each a
`RouterInterface {id, egress_gate, mac_addr, addresses}`. `id` is the Router
`InterfaceId`, which is `egress_gate + 1`; leave it 0 to derive it, and any
other value must equal it. `mac_addr` is text, `"aa:bb:cc:dd:ee:ff"`, as in
Vif, and must be nonzero. `addresses` is a list of the interface's own
addresses in CIDR form (`"192.0.2.1/24"`, `"2001:db8::1/64"`): any number of
either enabled family, at least one. Init refuses an address of a family the
router does not enable, a malformed address, and an interface without one.
The interfaces are fixed at Init; setting any enables neighbor resolution and
the `<router>/neighbors` resource.

### The `<router>/neighbors` resource

`<router>/neighbors` maps a next-hop id (`RouterNextHopIdKey {id}`) to a
`RouterNeighborBinding {interface_id, ipv4, ipv6}` with exactly one of `ipv4`
or `ipv6` set. Bindings come and go at runtime, in transactions like any
other resource:

- **Binding publishes the hop.** The committed transaction publishes the id
  as an incomplete next hop. The resolver takes the binding up on the next
  maintenance tick, and `neighbor_first_probe_ms` counts from that tick.
- **Validated before it is staged.** A binding must name a configured
  interface, an address of a family the router enables and the interface has
  an address of, and not one of the interface's own addresses
  (`NeighborResolver::CheckBind`). A refused binding refuses the transaction.
- **Static and bound ids are disjoint.** An id is in `<router>/next_hops` or
  in `<router>/neighbors`, never both; a transaction that would make it both
  is refused.
- **Refused while referenced.** Routes and groups name a bound id like any
  next hop and hold a reference to its binding, so an unbind is refused while
  any route or group names it. Remove (or repoint) them in the same or an
  earlier transaction.
- **Abort leaves nothing.** An aborted transaction leaves no binding, hop or
  pending resolver change behind.
- **Apply order.** Desired-state apply upserts resources by ascending rank
  and erases by descending rank, so a manifest may list `<router>/neighbors`
  after the routes that name its ids: bindings are upserted before routes
  and erased after them.
- **Capacity.** `max_neighbors` bounds the bindings (and so the neighbors
  resolved). It defaults to 1024 or `max_next_hops`, whichever is smaller;
  a value above `max_next_hops` is refused. A binding past it refuses the
  transaction.

Reading `<router>/neighbors` back returns the bindings in the same form, with
canonical address text. A router without interfaces has no resolver, so it
binds nothing.

### `get_neighbors`

The `get_neighbors` action (`EmptyArg` →
`RouterCommandGetNeighborsResponse`) lists every binding in id order as a
`RouterNeighborStatus {next_hop_id, interface_id, ipv4, ipv6, state,
mac_addr, reconfirming, probes}`. `state` is resolved, incomplete or
unreachable; `mac_addr` is empty until a MAC is learned; `reconfirming` marks
a resolved neighbor whose reachable lifetime ran out and which is being
probed while it still forwards; `probes` counts the solicitations of the
current attempt. A binding made since the last tick reads incomplete with no
probes. The action reads control-side state under the control-plane lock, so
it runs while workers run.

### Who does what in the graph

- **Ingress tagging.** The Router reads the interface an ARP/ND frame arrived
  on from `ingress_interface_attr` (default `vif_id`). A Vif writes that
  attribute by default, so give each Vif interface the same `id` as the Router
  interface it feeds (`egress_gate + 1`). Without a Vif, a `SetMetadata` on
  each ingress path writes it.
- **Answering for the router's own addresses.** The Router does not answer ARP
  requests or Neighbor Solicitations: it sends them to `punt_neighbor_gate`,
  or drops them, and counts them. `punt_neighbor_gate` 0 (the default) follows
  `punt_local_gate`, 8192 (`DROP_GATE`) drops them, and any other invalid
  gate is refused at Init. Configure the interface's `ip_addr`/`ipv6_addr` on
  its Vif (or put an ARP/ND responder on the gate) so neighbors can resolve
  the router.
- **What reaches the Router.** Only frames for the interface MAC should reach
  the Router, plus the Vif's own answers, which must be diverted before it
  (below). Give each Vif `default_gate: 8192` so a frame for any other MAC is
  dropped by the Vif rather than delivered.
- **Diverting the Vif's answers.** A Vif emits its ARP and NA answers on the
  interface's data gate, the same gate that feeds the Router. Divert them to
  the port before the Router, e.g. with an `ExactMatch` on the Ethernet source
  MAC (an answer carries the interface's own MAC). Otherwise the Router takes
  them as input and drops them.

### Examples

A family is enabled exactly when its route capacity (`max_ipv4_routes`,
`max_ipv6_routes`) is set; at least one must be. Interface addresses and
bindings must be of an enabled family. The three Router configurations
below differ only in that; the graph around them is the one in the dual-stack
example.

IPv4 only (`rte_lpm`, 256 tbl8 groups per domain by default):

```yaml
  - name: rt
    mclass: Router
    arg:
      mode: ROUTE_MODE_DESTINATION
      max_ipv4_routes: 1024
      interfaces:
        - {egress_gate: 0, mac_addr: "02:00:00:00:00:01", addresses: ["192.0.2.1/24"]}
        - {egress_gate: 1, mac_addr: "02:00:00:00:00:02", addresses: ["198.51.100.1/24"]}
resources:
  rt/neighbors:
    - {key: {id: 1}, value: {interface_id: 2, ipv4: "198.51.100.254"}}
  rt/routes:
    - {key: {ipv4: "203.0.113.0", prefix_length: 24}, value: {next_hop_id: 1}}
```

IPv6 only (`rte_fib6`; `max_ipv6_tbl8s` sizes each domain, see
docs/routing.md):

```yaml
  - name: rt
    mclass: Router
    arg:
      mode: ROUTE_MODE_DESTINATION
      max_ipv6_routes: 1024
      max_ipv6_tbl8s: 1024
      interfaces:
        - {egress_gate: 0, mac_addr: "02:00:00:00:00:01", addresses: ["2001:db8:1::1/64"]}
        - {egress_gate: 1, mac_addr: "02:00:00:00:00:02", addresses: ["2001:db8:2::1/64"]}
resources:
  rt/neighbors:
    - {key: {id: 2}, value: {interface_id: 2, ipv6: "2001:db8:2::fe"}}
  rt/routes:
    - {key: {ipv6: "2001:db8:100::", prefix_length: 48}, value: {next_hop_id: 2}}
```

Dual-stack, with the whole graph: two ports, one destination-mode Router, an
IPv4 and an IPv6 neighbor on the second port, and one route of each family to
them:

```yaml
ports:
  - {name: p0, driver: PMDPort, arg: {port_id: 0}}
  - {name: p1, driver: PMDPort, arg: {port_id: 1}}
modules:
  - {name: in0, mclass: PortInc, arg: {port: p0}}
  - {name: in1, mclass: PortInc, arg: {port: p1}}
  # Vif interface id == Router interface id == egress_gate + 1. A frame for
  # another MAC takes the default gate, 8192: dropped.
  - name: vif0
    mclass: Vif
    arg:
      default_gate: 8192
      interfaces:
        - {id: 1, mac_addr: "02:00:00:00:00:01", ip_addr: "192.0.2.1", ipv6_addr: "2001:db8:1::1", gate: 0}
  - name: vif1
    mclass: Vif
    arg:
      default_gate: 8192
      interfaces:
        - {id: 2, mac_addr: "02:00:00:00:00:02", ip_addr: "198.51.100.1", ipv6_addr: "2001:db8:2::1", gate: 0}
  # The Vif's own answers carry the interface MAC as their source: send them
  # to the port (gate 0), everything else to the Router (gate 1).
  - {name: own0, mclass: ExactMatch, arg: {fields: [{offset: 6, num_bytes: 6}], default_gate: 1}}
  - {name: own1, mclass: ExactMatch, arg: {fields: [{offset: 6, num_bytes: 6}], default_gate: 1}}
  - name: rt
    mclass: Router
    arg:
      mode: ROUTE_MODE_DESTINATION
      max_ipv4_routes: 1024
      max_ipv6_routes: 1024
      max_ipv6_tbl8s: 1024
      interfaces:
        - {egress_gate: 0, mac_addr: "02:00:00:00:00:01", addresses: ["192.0.2.1/24", "2001:db8:1::1/64"]}
        - {egress_gate: 1, mac_addr: "02:00:00:00:00:02", addresses: ["198.51.100.1/24", "2001:db8:2::1/64"]}
  - {name: out0, mclass: PortOut, arg: {port: p0}}
  - {name: out1, mclass: PortOut, arg: {port: p1}}
connections:
  - {upstream: in0, downstream: vif0}
  - {upstream: in1, downstream: vif1}
  - {upstream: vif0, downstream: own0}
  - {upstream: vif1, downstream: own1}
  - {upstream: own0, downstream: out0}
  - {upstream: own1, downstream: out1}
  - {upstream: own0, downstream: rt, ogate: 1}
  - {upstream: own1, downstream: rt, ogate: 1}
  - {upstream: rt, downstream: out0}
  - {upstream: rt, downstream: out1, ogate: 1}
resources:
  own0/rules:
    - {key: {fields: [{value_bin: "AgAAAAAB"}]}, value: {gate: 0}}
  own1/rules:
    - {key: {fields: [{value_bin: "AgAAAAAC"}]}, value: {gate: 0}}
  rt/neighbors:
    - {key: {id: 1}, value: {interface_id: 2, ipv4: "198.51.100.254"}}
    - {key: {id: 2}, value: {interface_id: 2, ipv6: "2001:db8:2::fe"}}
  rt/routes:
    - {key: {ipv4: "203.0.113.0", prefix_length: 24}, value: {next_hop_id: 1}}
    - {key: {ipv6: "2001:db8:100::", prefix_length: 48}, value: {next_hop_id: 2}}
```

The interfaces omit `id`, so each is derived from its gate (1 and 2), matching
the Vif ids. No punt gate is set, so ARP requests and Neighbor Solicitations
that reach the Router are dropped and counted; the Vifs answer for the
interface addresses. A packet-path workload also needs workers; start from
`bessctl get` (docs/control-sdk.md).

### Handoff, timings and bounds

The Router hands control frames from workers to a bounded ring; its
maintenance tick calls `OnArp`, `OnNd` and `Poll` with a bounded work budget,
then publishes complete next-hop objects transactionally. A Router task emits
queued solicitation frames on the interface's egress gate. `max_neighbors`
bounds resolver state (default 1024 or `max_next_hops`, whichever is
smaller).

Timings are configured in milliseconds and counted in maintenance ticks: Init
converts each to ticks from the daemon's `--maintenance_interval_us`, rounding
up, so a configured N ms is at least N ms (and may be up to one tick more).
The resolver's clock starts at the tick current at Init. Init refuses a
resolver when the maintenance loop is disabled (`--maintenance_interval_us=0`),
since no neighbor would ever be solicited.

| field | unset | explicit 0 |
|---|---|---|
| `neighbor_first_probe_ms` | 1000 | solicit on the first tick |
| `neighbor_probe_interval_ms` | 1000 | refused |
| `neighbor_max_probe_interval_ms` | 8000 | refused (also refused below the probe interval) |
| `neighbor_max_probes` (a count) | 3 | refused |
| `neighbor_reachable_lifetime_ms` | 30000 | never re-solicit a resolved neighbor |
| `neighbor_unreachable_retry_ms` | 8000 (the backoff ceiling) | never retry an unreachable neighbor |

The receive and solicitation rings each hold 1024 entries and the ARP request
copy ring 256; a full ring drops that control frame or solicitation and the
timer retry keeps behavior bounded. Each maintenance tick handles at most 64
received frames and polls at most 64 timer records. An idle Router output
task blocks until it is scheduled again.

### Current limits

- Interfaces are fixed at Init: adding, removing or readdressing one means
  recreating the Router. Bindings are runtime state (`<router>/neighbors`).
- No glean: route gateways and connected hosts are not resolved on demand. A
  route resolves only through a bound next-hop id, so every neighbor (each
  host on a connected subnet included) needs a binding. Resolving on demand
  would need the packet path to create neighbor entries from traffic, which
  lets any sender grow the table and costs the miss path a control handoff;
  explicit bindings keep the table bounded and owned by the controller.

## What moved out of Vif, and what stayed

Moved: the hand-written ARP reply. `Vif` now calls
`route::AnswerArpRequest(frame, iface_mac, iface_ip)` from
`route/l3_packet.h`, which parses the frame with `ParseArp` and builds the
reply with `BuildArpReply`. The packet, the fields and the gate are unchanged,
so `VifTest.LocalArpResponder` still pins the behavior.

Stayed: VLAN tag matching and stripping, the (VLAN, MAC) demultiplexer, the
(VLAN, address) local-responder lookup, metadata writes and the gate choice.
`Vif` is an L2/L3 interface adapter; it does not resolve, and it should not
grow timers or a second ARP implementation again.

## IPv6 ND

IPv6 uses the same entries, table, timers and publication path (D-176); only
the packet family and the address in the key differ. `OnNd` accepts a
Neighbor Advertisement and `Poll` emits a Neighbor Solicitation with the
source link-layer option when the interface has an IPv6 address and a MAC.

An advertisement is accepted only when all of the following hold:

- it is a valid ND frame for the ingress interface: Ethernet/IPv6 framing,
  version 6, next header ICMPv6, hop limit 255, a correct ICMPv6 checksum
  over the pseudo-header, an ND type, no malformed option;
- the destination is this generation's: either the Ethernet destination is
  the interface's MAC and the IPv6 destination is one of its current IPv6
  addresses (solicited), or both are all-nodes (the unsolicited MAC-change
  form); a multicast destination requires the Solicited flag to be clear (RFC
  4861 7.1.1), and an interface with no IPv6 address never matches a
  unicast destination; the
  source/target link-layer option, if present, equals the Ethernet source,
  and the sender MAC is unicast, non-zero and not the interface's own;
- the source address is not multicast or unspecified and is not the loopback
  address; the target is not multicast or unspecified;
- source and target are the same address (proxy advertisements are not
  supported), so the target names the sender actually on the link;
- the ingress interface and, if the entry already exists, its generation
  match, and the target has an entry (only `Bind` creates one). Moving the
  interface's own IPv6 address therefore rejects advertisements still sent to
  the old address, even on the same MAC.

A solicited advertisement updates the MAC. An unsolicited one updates an
existing resolved entry only when its override flag is set or the MAC is
unchanged; otherwise it is ignored, which is RFC 4861's protection against a
gratuitous advertisement moving traffic. Link-local and global unicast
addresses are both accepted; there is no scope/zone field, so an application
that needs a non-link-local scope must use distinct interfaces or addresses.

Solicitations are sent to the target's solicited-node multicast address
(`ff02::1:ffXX:XXXX`, MAC `33:33:ff:XX:XX:XX`), from the interface address
whose prefix holds the target (else its first IPv6 address). `Vif` answers a solicitation for its local address with
`AnswerNeighborSolicitation`, setting S for a normal solicitation and
answering a duplicate-address-detection solicitation (source `::`, no source
link-layer option, destination the target's solicited-node group) to
all-nodes with S clear. Every answer carries the target link-layer option:
RFC 4861 7.2.4 requires it because the solicitation's destination is
multicast, so a 78-byte DAD solicitation produces an 86-byte answer and
`Vif` grows the packet into its tailroom. That responder is still packet
mechanics, not resolution: `Vif` does not learn from a solicitation and never
creates or updates a neighbor entry.

The configuration API adds `ipv6_addr` to a Vif interface, alongside
`ip_addr`; the readback emits it when set. The Vif `ipv6_addr` does not
participate in demultiplexing (the interface still matches by VLAN and
destination MAC) or in metadata.
