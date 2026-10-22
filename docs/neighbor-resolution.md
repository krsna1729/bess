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

Interface identity is explicit: `ConfigureInterface({id, mac, ipv4, ipv6})`
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
back if the wheel refuses the new timer.

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
  rest wait for the next tick (D-179). Its next-hop resource rejects ordinary
  writes and erases for configured resolver-owned ids, and its codec does not
  enumerate them, so desired-state convergence of `<router>/next_hops` leaves
  them alone while a manifest naming one is refused.

Aging does not stop forwarding: when a resolved neighbor's reachable lifetime
expires it stays resolved, with its MAC, while the resolver probes it with the
normal backoff, and becomes unreachable only after `max_probes` unanswered
probes (RFC 4861 STALE/PROBE). A reply during reconfirmation keeps it resolved
without republishing. In the `Router` module each `neighbor_*` field is
optional: unset takes the default, and an explicit 0 has one meaning per field
(listed below). ARP replies and Neighbor
Advertisements are queued with the interface id read when they arrive (an
unknown interface or a full queue drops and counts them); ARP requests and
Neighbor Solicitations are not resolver input and take the local punt gate, or
are dropped, and are counted.

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
`RouterInterface {id, egress_gate, mac_addr, ipv4, ipv6}`. `id` is the Router
`InterfaceId`, which is `egress_gate + 1`; leave it 0 to derive it, and any
other value must equal it. `mac_addr` is text, `"aa:bb:cc:dd:ee:ff"`, as in
Vif, and must be nonzero; `ipv4`/`ipv6` are the local source addresses of its
solicitations, and a binding of a family needs the interface to have that
family's address. `neighbor_bindings` binds a `next_hop_id` to an interface
and exactly one IPv4 or IPv6 peer address. Those bindings seed incomplete next
hops and own those ids; routes may refer to them, but a static
`RouterNextHopValue` must not be written for them.

### Who does what in the graph

- **Ingress tagging.** The Router reads the interface an ARP/ND frame arrived
  on from `ingress_interface_attr` (default `vif_id`). A Vif writes that
  attribute by default, so give each Vif interface the same `id` as the Router
  interface it feeds (`egress_gate + 1`). Without a Vif, a `SetMetadata` on
  each ingress path writes it.
- **Answering for the router's own addresses.** The Router does not answer ARP
  requests or Neighbor Solicitations: it sends them to `punt_local_gate`, or
  drops them, and counts them. Configure the interface's `ip_addr`/`ipv6_addr`
  on its Vif (or put an ARP responder on the path) so neighbors can resolve
  the router.
- **Diverting the Vif's answers.** A Vif emits its ARP and NA answers on the
  interface's data gate, the same gate that feeds the Router. Divert them to
  the port before the Router, e.g. with an `ExactMatch` on the Ethernet source
  MAC (an answer carries the interface's own MAC). Otherwise the Router takes
  them as input and drops them.

### Example

Two ports, one destination-mode Router, an IPv4 and an IPv6 neighbor on the
second port, and one route of each family to them:

```yaml
ports:
  - {name: p0, driver: PMDPort, arg: {port_id: 0}}
  - {name: p1, driver: PMDPort, arg: {port_id: 1}}
modules:
  - {name: in0, mclass: PortInc, arg: {port: p0}}
  - {name: in1, mclass: PortInc, arg: {port: p1}}
  # Vif interface id == Router interface id == egress_gate + 1.
  - name: vif0
    mclass: Vif
    arg:
      interfaces:
        - {id: 1, mac_addr: "02:00:00:00:00:01", ip_addr: "192.0.2.1", ipv6_addr: "2001:db8:1::1", gate: 0}
  - name: vif1
    mclass: Vif
    arg:
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
      interfaces:
        - {egress_gate: 0, mac_addr: "02:00:00:00:00:01", ipv4: "192.0.2.1", ipv6: "2001:db8:1::1"}
        - {egress_gate: 1, mac_addr: "02:00:00:00:00:02", ipv4: "198.51.100.1", ipv6: "2001:db8:2::1"}
      neighbor_bindings:
        - {next_hop_id: 1, interface_id: 2, ipv4: "198.51.100.254"}
        - {next_hop_id: 2, interface_id: 2, ipv6: "2001:db8:2::fe"}
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
  rt/routes:
    - {key: {ipv4: "203.0.113.0", prefix_length: 24}, value: {next_hop_id: 1}}
    - {key: {ipv6: "2001:db8:100::", prefix_length: 48}, value: {next_hop_id: 2}}
```

The interfaces omit `id`, so each is derived from its gate (1 and 2), matching
the Vif ids. A packet-path workload also needs workers; start from
`bessctl get` (docs/control-sdk.md).

### Handoff, timings and bounds

The Router hands control frames from workers to a bounded ring; its
maintenance tick calls `OnArp`, `OnNd` and `Poll` with a bounded work budget,
then publishes complete next-hop objects transactionally. A Router task emits
queued solicitation frames on the interface's egress gate. `max_neighbors`
bounds resolver state (default 1024).

Timings are milliseconds; Init converts them to maintenance ticks from the
daemon's `--maintenance_interval_us`, rounding up, and the resolver's clock
starts at Init. Init refuses a resolver when the maintenance loop is disabled
(`--maintenance_interval_us=0`), since no neighbor would ever be solicited.

| field | unset | explicit 0 |
|---|---|---|
| `neighbor_first_probe_ms` | 1000 | solicit on the first tick |
| `neighbor_probe_interval_ms` | 1000 | refused |
| `neighbor_max_probe_interval_ms` | 8000 | refused (also refused below the probe interval) |
| `neighbor_max_probes` (a count) | 3 | refused |
| `neighbor_reachable_lifetime_ms` | 30000 | never re-solicit a resolved neighbor |
| `neighbor_unreachable_retry_ms` | 8000 (the backoff ceiling) | never retry an unreachable neighbor |

The receive and solicitation rings each hold 1024 entries; a full ring drops
that control frame or solicitation and the timer retry keeps behavior bounded.
Each maintenance tick handles at most 64 received frames and polls at most 64
timer records. An idle Router output task blocks until it is scheduled again.

### Current limits

- Interfaces and bindings are fixed at Init: adding or removing a neighbor
  means recreating the Router.
- Route gateways are not resolved on demand: a route resolves only through a
  bound next-hop id, so every neighbor (each host on a connected subnet
  included) needs a binding.
- Neighbor state is not inspectable yet: bound hops are hidden from
  `<router>/next_hops`, so a route read-back can name an id the next-hop
  read-back omits; GetDesc shows only counts.
- ARP requests and Neighbor Solicitations share `punt_local_gate` with local
  delivery; there is no separate neighbor punt gate.
- Gratuitous ARP is not learned: an IPv4 MAC move is noticed only when the
  neighbor is next reconfirmed.
- One address per family per interface, without a prefix length.

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
  the interface's MAC and the IPv6 destination is its current address
  (solicited), or both are all-nodes (the unsolicited MAC-change form); a
  multicast destination requires the Solicited flag to be clear (RFC 4861
  7.1.1), and an interface with no IPv6 address (`::`) never matches a
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
(`ff02::1:ffXX:XXXX`, MAC `33:33:ff:XX:XX:XX`), with the interface's own
address as source. `Vif` answers a solicitation for its local address with
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
