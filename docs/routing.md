# Routing in BESS

The router library (`core/route/`) is a complete IPv4 router: per-domain longest-prefix FIBs, next hops with
neighbor state and link addresses, ECMP groups, a neighbor table whose resolution events are next-hop updates,
and the packet mechanics (TTL, MTU, ICMPv4, ARP). Two callers use it: the reference appliance R1
(`examples/appliances/router/`, an application) and the `Router` module (`core/modules/router.cc`, a graph
adapter). This is the guide to the pieces and to the two ways a graph uses them.

## The machine, in five steps

1. **A frame arrives.** The interface it arrived on decides the routing context: a **route domain** (a VRF; in
   3GPP terms a network instance). A packet carries it as the `route_domain` metadata a VIF writes, or the
   caller passes it (an interface's VRF).
2. **Look the destination up** in that domain's FIB: longest prefix match. `10.1.2.3` matches `0/0`, `10/8`,
   `10.1/16`, `10.1.2/24`; the longest wins.
3. **The entry names a next hop.** The next hop's egress interface, neighbor state and link addresses are the
   **adjacency**: "write these MACs, send out this interface."
4. **Per-hop duties**: decrement the TTL (and fix the IPv4 checksum); a graph that enforces an MTU checks it
   here (`CheckMtu` is the library's helper for that; the Router module does not do it).
5. **Egress.** If the neighbor's MAC is not known, the policy decides: drop, queue (which triggers the
   resolution), or punt to a slow path. BESS leaves that choice to the application.

## RIB and FIB: BESS has the FIB

A **RIB** is what routes exist and which one is best — built by a control plane from connected interfaces,
static configuration and protocols (OSPF/BGP/IS-IS), with preference and metrics breaking ties, and recursion
resolving a next hop that is itself reached by another route. A **FIB** is the distilled lookup the dataplane
consults per packet: prefix → next hop(s), only for usable entries, as an LPM structure (DPDK `rte_lpm` /
`rte_fib`; Linux `fib_trie`; VPP's mtrie).

BESS has no RIB. `LpmRouteTable` *is* the FIB, and whoever programs it — a controller through a transaction, an
application, a netlink mirror — is playing the RIB's role. That is the same position VPP and Linux-with-FRR
take, except that they at least offer a protocol daemon as a client. There is no best-path selection, no
preference, no recursion: a route names what it means.

## Route → next hop → adjacency

The three-layer decomposition is the part worth internalizing:

| layer | question | content | changes when |
|---|---|---|---|
| FIB entry | which prefix? | prefix → next-hop id or group id | a route changes |
| next hop | who do I send to? | egress interface, neighbor address, neighbor state | a route or a neighbor is added |
| adjacency | what bytes go on the wire? | source MAC, destination MAC | the neighbor's MAC changes |

BESS has exactly this split: `RouteKey{domain, prefix}` → a next-hop id, and
`NextHop{egress, neighbor, dst_mac, src_mac}`. When a neighbor resolves, `NeighborTable::Update` returns the
next hops to republish and the application calls `Router::SetNextHop`; **routes never move**. An ARP reply is
one object store, not a route update, so there is no churn and no transient hole. A route to a next-hop group
is the same shape one level up: the FIB names the group, and the group's member is chosen by the frame's own
flow hash (`route::FlowHash`), so a flow keeps one member (D-156).

A next hop whose egress interface is absent (`kInvalidInterfaceId`; on the module's wire, `egress_gate =
DROP_GATE`) forwards nowhere: the chain reports `kNoEgress` and the packet ends there. That is the blackhole
shape -- `0.0.0.0/0` to such a hop is the domain's default that drops, and a local `/32` uses it for the
node's own address -- and the module counts what it drops as `blackholed`, apart from `no route`, because the
route exists and chose that hop (D-160).

## The pieces

| file | what it is |
|---|---|
| `route/route_table.h` | the FIB: `LpmRouteTable` (rte_lpm or rte_fib, D-132), lock-free lookups, RCU publication, the `/0` route kept beside the structure |
| `route/router.h` | the router: route domains, per-domain FIBs, next hops, next-hop groups, `Resolve`/`ResolveBatch`, the direct setters and the transactional resource (`Enroll`) |
| `route/neighbor_table.h` | neighbor state keyed by (interface, address); `Bind`/`Update` return the next hops to republish |
| `route/l3_packet.h` | packet mechanics: `DecrementTtl`, `FlowHash`, MTU, ICMPv4 errors, ARP build/parse |
| `route/forward.h` | the IPv4 forwarding chain both callers share (D-157): `ResolveFrame` (read) and `WriteForwarded` (write) |
| `route/lpm_resource.h` | one `LpmRouteTable` as a transaction resource (D-111): prefix → an opaque value whose meaning is the module's |
| `conntrack/packet_parse.h` | the checked parse the chain starts from |

## Two recipes

**Recipe A — policy forwarding (sessions).** A classifier (or a PDR/selector) picks the next hop, writes it as
`next_hop_id` metadata, and the Router resolves it and leaves on that hop's egress gate:

```
ports/vifs -> classifier -> ActionTable (next_hop_id) -> Router -> ports -> UpdateTTL
```

No routes are written; the router is a next-hop mux, and it writes no TTL -- this is the mode whose contract is
byte preservation apart from the addresses, so the graph's `UpdateTTL` decrements it. This is the shape the UPF
and OMEC use today, and it stays first-class.

**Recipe B — a routed network.** Ports and VIFs feed the Router, which looks each packet's own destination up
in its route domain and forwards it:

```
ports/vifs -> Router (domains, routes, groups, next hops) -> ports
```

This is R1 written as a graph, and it is what replaces `IPLookup`. The chain decrements the TTL and fixes the
checksum itself in this mode (D-155), so `UpdateTTL` has no place in this graph: wired as drawn above, it would
decrement twice. The Router carries neighbor *state*, not resolution: nothing in the library sends an ARP or ND
request (D-153) -- a graph's own resolution component resolves an address and republishes the affected next
hops (`NeighborTable::Update` -> `Router::SetNextHop`), and what an unresolved neighbor means stays policy.
Routes, next hops and groups are written through the module's resources (`<router>/routes`, `/next_hops`,
`/groups`) in transactions, or directly by an application.

The two recipes meet in one module. Which one runs is **configuration**, not a per-packet test: the `mode` arg
(`ROUTE_MODE_NEXT_HOP_ID`, the default, or `ROUTE_MODE_DESTINATION`, D-155). A module decides; it does not
change what it decides from because a graph's wiring changed underneath it. In destination mode the
`next_hop_id` attribute is ignored.

## What the module decides, and what stays with the application

The module owns: the mode, the gate map (`modules/router_gate_map.h` — a hop's egress interface to an output
gate), the counters (one per outcome of the chain: not IPv4, malformed, non-initial fragment, no route,
unresolved neighbor, TTL expired, a hop that forwards nowhere (D-160), header-write failure), and the copy a
shared payload needs before its header is written.

The application owns everything that is policy: which domain a packet is in (an interface, a VIF, a port), where
the TTL is decremented (`UpdateTTL` in the graph, or the chain when the module routes by destination), and
ICMP/uRPF (not the Router's business). A packet whose destination has no route and a packet whose neighbor is
unresolved are distinct, counted outcomes; the module drops both by default, and it can send either to a gate
instead (`punt_miss_gate`, `punt_unresolved_gate`, D-162) -- the frame is counted either way, so the counters
stay the record of what happened.

## Domains are cheap: `max_domains` is a bound

`max_domains` bounds the domain ids; it does not allocate them. A domain's FIB is created when it is first
needed — explicitly (`CreateDomain`), or by the first route a transaction writes into it (D-158). A router
configured for four network instances pays for the instances that hold routes, not for the configuration.
`RemoveDomain` takes an empty domain away; removal is frozen once the router is enrolled, because the engine's
resource holds raw FIB pointers (D-046).

## IPLookup is retired

`IPLookup` was the legacy shape: one table per module, gate-valued routes, no domains, no next hops, no
addresses. Everything it could express, the Router expresses with more (D-155, D-156), so the module, its
resource table and its three protobuf messages are gone; a manifest that names `mclass: IPLookup` no longer
applies, and `docs/compatibility.md` carries the entry a user reads (D-161). The library class its routes used
(`route/lpm_resource.h`, D-111) stays: a prefix table a transaction can change is the shape a subscriber
lookup needs when framed routes and IPv6 prefixes arrive (prefix → session id), and it is cheaper to reuse
than to grow a second prefix table.

## Where to look next

- `docs/dataplane-tables.md` — the tables themselves, their capacities and their update costs.
- `docs/decisions.md` — D-046 (domains), D-111 (the LPM resource), D-132 (the FIB backends), D-153 (address
  writes), D-155 (routing by destination), D-156 (groups), D-157 (the shared chain), D-158 (lazy domains),
  D-160 (a hop that forwards nowhere).
- `examples/appliances/router/router_app.h` — R1, the reference application, and `examples/static_router` for
  the same code as a program without `bessd`.
