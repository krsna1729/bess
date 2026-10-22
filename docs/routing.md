# Routing in BESS

The router library (`core/route/`) provides per-domain dual-stack longest-prefix FIBs, next hops with
neighbor state and link addresses, ECMP groups, and IPv4/IPv6 packet mechanics. `NeighborResolver` supplies
ARP and ND state transitions. The reference appliance R1 (`examples/appliances/router/`, an application) and
the `Router` module (`core/modules/router.cc`, a graph adapter) use the route library. The Router module also
drives the resolver through its maintenance and task paths.

## The machine, in five steps

1. **A frame arrives.** The interface it arrived on decides the routing context: a **route domain** (a VRF; in
   3GPP terms a network instance). A packet carries it as the `route_domain` metadata a VIF writes, or the
   caller passes it (an interface's VRF).
2. **Look the destination up** in that domain's IPv4 or IPv6 FIB: longest prefix match. The most-specific
   matching prefix wins.
3. **The entry names a next hop.** The next hop's egress interface, neighbor state and link addresses are the
   **adjacency**: "write these MACs, send out this interface."
4. **Per-hop duties**: decrement the IPv4 TTL (and fix its header checksum), or decrement the IPv6 hop limit;
   a graph that enforces an MTU checks it here (`CheckMtu` is the library's helper for IPv4 only).
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
flow hash (`route::FlowHash`), so a flow keeps one member (D-156); which fields that hash reads is the graph's
choice -- the whole flow by default, the address pair alone with `FLOW_HASH_L3` (D-163).

A next hop whose egress interface is absent (`kInvalidInterfaceId`; on the module's wire, `egress_gate =
DROP_GATE`) forwards nowhere: the chain reports `kNoEgress` and the packet ends there. That is the blackhole
shape -- `0.0.0.0/0` to such a hop is the domain's default that drops, and a local `/32` uses it for the
node's own address -- and the module counts what it drops as `blackholed`, apart from `no route`, because the
route exists and chose that hop (D-160).

## The pieces

| file | what it is |
|---|---|
| `route/route_table.h` | the FIB: `LpmRouteTable` (rte_lpm or rte_fib, D-132) for IPv4 and `Ipv6RouteTable` (rte_fib6, two instances, D-180) for IPv6, lock-free lookups, the `/0` route kept beside the structure |
| `route/router.h` | the router: route domains, per-domain dual-stack FIBs, next hops, next-hop groups, `Resolve`/`ResolveBatch`, the direct setters and the transactional resource (`Enroll`) |
| `route/neighbor_table.h` | neighbor state keyed by (interface, address family, address); `Bind`/`Update` return the next hops to republish |
| `route/neighbor_resolver.h` | the IPv4/IPv6 neighbor state machine: ARP or ND solicitation, retries/backoff, aging, MAC change, failure/recovery, interface identity; returns frames and neighbor updates (`docs/neighbor-resolution.md`) |
| `route/l3_packet.h` | packet mechanics: IPv4 TTL/checksum, IPv6 hop limit, `FlowHash`, MTU, ICMPv4 errors, ARP and IPv6 ND build/parse |
| `route/forward.h` | the dual-stack forwarding chain both callers share (D-157, D-180): `ResolveFrame(s)` (read) and `WriteForwarded` (write), IPv4 and IPv6 through one implementation; link-local/multicast IPv6 is never routed |
| `route/connected_routes.h` | the routes an interface address implies (D-178): `DerivedRoutes::Derive` for connected + local, deterministic order and stable keys, `InstallationOps`/`EraseOps` for the controller's transaction |
| `route/lpm_resource.h` | one IPv4 `LpmRouteTable` as a transaction resource (D-111): prefix → an opaque value whose meaning is the module's |
| `conntrack/packet_parse.h` | the checked parse the chain starts from |

## Two recipes

**Recipe A — policy forwarding (per-flow next hops).** A classifier picks the next hop, writes it as
`next_hop_id` metadata, and the Router resolves it and leaves on that hop's egress gate:

```
ports/vifs -> classifier (next_hop_id) -> Router -> ports
```

The Router decrements the IPv4 TTL and fixes its checksum, or decrements the
IPv6 hop limit, before the L2 rewrite, exactly as destination mode does (D-153,
D-180); a frame arriving with a hop count of 0 or 1 is dropped and counted as
TTL expired. No FIB routes are needed: the classifier selects a hop, and the
Router resolves it to an egress gate.

**Recipe B — a routed network.** Ports and VIFs feed the Router, which looks each packet's own destination up
in its route domain and forwards it:

```
ports/vifs -> Router (domains, routes, groups, next hops) -> ports
```

The chain decrements the IPv4 TTL and fixes its checksum, or decrements the IPv6 hop limit, in this mode
(D-155). The Router module's configured NeighborResolver processes ARP/ND input and timers on its control
maintenance path, publishes whole next-hop objects through transactions, and emits solicitation frames from
its task on the configured output gate. An unresolved neighbor remains fail-closed until a valid reply or
advertisement updates it. Routes, next hops, neighbor bindings and groups use the module resources
(`<router>/routes`, `/next_hops`, `/neighbors`, `/groups`) in transactions, or direct library APIs.
For IPv4, `route::ConnectedRouteFor(address, length, egress)` returns an
interface prefix with an unresolved next hop (D-164). IPv6 routes are
configured with `RouterRouteKey.ipv6` and an `Ipv6Prefix`; they share the
same route domain and next-hop resources. The Router does not synthesize
connected IPv6 routes.

`RouterArg.domain_attr` defaults to `route_domain`; several Routers can read
different domain attributes, e.g. an overlay and an underlay FIB. A
`RouterRouteKey` sets exactly one of `ipv4` or `ipv6` (with `prefix_length`
and `domain`), so an IPv6 route is written as `{ipv6: "2001:db8::",
prefix_length: 32}`. The ingress interface attribute (`ingress_interface_attr`,
default `vif_id`, what Vif writes) is independent of the route domain. A
binding in `<router>/neighbors` owns its next-hop id: routes and groups name
it, a static `<router>/next_hops` entry for the same id is refused, and an
unbind is refused while a route or group names it. The interface id is
the Router's logical `InterfaceId` (`egress_gate + 1`), not a queue id or any
other ingress tag; the graph must tag ARP/ND input with that id.
docs/neighbor-resolution.md has the resource's rules, `get_neighbors`, and
IPv4-only, IPv6-only and dual-stack configuration examples.

## Address families and sizing

`RouterArg` enables each family on its own: a family is on exactly when its
route capacity is set.

| field | meaning | default |
|---|---|---|
| `max_ipv4_routes` | IPv4 routes per domain; setting it enables IPv4 | unset: IPv4 off |
| `max_ipv4_tbl8s` | IPv4 tbl8 groups per domain, one per /24 holding a longer route | 256 |
| `max_ipv6_routes` | IPv6 routes per domain; setting it enables IPv6 | unset: IPv6 off |
| `max_ipv6_tbl8s` | IPv6 (rte_fib6) tbl8 groups per domain; a route reserves one per byte between its length and its covering route's, counted from /24 | 8192 |

Init refuses: no family enabled ("set max_ipv4_routes, max_ipv6_routes or
both"); an explicit 0 for any of the four; a `max_*_tbl8s` whose family's
route capacity is unset; a capacity past 32 bits; a non-default `backend`
(which selects the IPv4 table) without IPv4; and an interface address of a
disabled family. A disabled family has no FIB in any domain and costs
nothing. In both modes its frames are dropped and counted as
`family_disabled_frames` (GetDesc: `family disabled`), its routes and
neighbor bindings are refused, and its neighbor protocol (ARP or ND) is not
read.

**IPv6 on rte_fib6, two instances per domain.** rte_fib6's trie cannot change
a rule in place without possibly allocating: rewriting a rule takes a
transient tbl8 group and folds it back, and a rule added with its parent's
value takes its groups on a later change, when the pool may be empty while
freed groups wait for a grace period. A change or delete at publication could
then fail half written. So each domain's IPv6 table owns a `live` instance,
which readers use through RCU and nobody edits, and a `spare` that a
transaction edits (every allocation and capacity check happens there).
Commit is one pointer exchange; the old live becomes the spare after a grace
period and replays the prefixes it lags by. An aborted transaction changes
nothing, and publication allocates nothing.

Memory per IPv6 domain follows from that. One instance is the 2^24-entry
tbl24 plus `max_ipv6_tbl8s` × 256 entries plus the rib's 2 × `max_ipv6_routes`
nodes. Entries are 2 bytes while `max_next_hops + max_groups` and
`max_ipv6_tbl8s` both fit in 32,767, else 4 bytes. At 2-byte entries an
instance is 32 MiB + 512 B × `max_ipv6_tbl8s` (36 MiB at the default 8192),
and a domain holds one instance until its first change and two after: about
2 × (32 MiB + 512 B × `max_ipv6_tbl8s`) plus the rib, double that at 4-byte
entries. Lower `max_ipv6_tbl8s` to what the route set needs when many domains
carry IPv6.

An interface address also implies a **local route** (D-178): the address itself is this node's, so a packet to
it is delivered here and never forwarded. `route::DerivedRoutes::Derive(interfaces)` returns both routes per
address -- the connected subnet and the local `/32` -- in a deterministic order with a stable key
`(domain, prefix)`, collapsing duplicate inputs. A `/32` address derives only the local route: the connected
subnet and the address's own host route would be the same key, and the local meaning is the honest one. Two
interfaces deriving the same key in one domain is refused as a conflict; overlapping (but different) prefixes
are both derived and the FIB's longest-prefix match decides. `InstallationOps(router, entries, hop_ids)` builds
the next-hop and route upserts for one transaction, and `StaleRouteKeys(previous, next)` plus `EraseOps` name
what an address move or removal leaves behind. The library never writes a table: the controller assigns the ids
and applies the transaction, so a refused transaction leaves no partial derived state and the same derivation
can be retried.

A local hop (`route::LocalNextHop()`) has kind `kLocal`, no egress interface and no addresses. The forwarding
chain reports `kLocalDelivery` for it before it consults either the egress or the neighbor; a caller decides
where a local packet goes. The Router module's destination mode sends it to `punt_local_gate` (default: drop)
and counts it as `local`, apart from a miss, an unresolved neighbor and a blackhole. That is the local
delivery path in a dataplane graph: there is no kernel stack to hand the packet to, so the graph names the slow
path. A local hop behind a `next_hop_id` is not forwarded either; the id mode has no punt gates for forwarded
traffic (D-162). The one gate it uses is `punt_neighbor_gate`: a resolving router sends the ARP requests and
Neighbor Solicitations it receives there in either mode (0, the default, follows `punt_local_gate`; 8192
drops them).

The two recipes meet in one module. Which one runs is **configuration**, not a per-packet test: the `mode` arg
(`ROUTE_MODE_NEXT_HOP_ID`, the default, or `ROUTE_MODE_DESTINATION`, D-155). A module decides; it does not
change what it decides from because a graph's wiring changed underneath it. In destination mode the
`next_hop_id` attribute is ignored.

## What the module decides, and what stays with the application

The module owns: the mode, the gate map (`modules/router_gate_map.h` — a hop's egress interface to an output
gate), the counters (one per outcome of the chain: not IP, family disabled, link-scoped IPv6, malformed,
non-initial fragment, no route, unresolved neighbor, TTL expired, a hop that forwards nowhere (D-160), a local
address (D-178), header-write failure), and the copy a shared payload needs before its header is written.

The application owns everything that is policy: which domain a packet is in (an interface, a VIF, a port), where
the TTL is decremented (`UpdateTTL` in the graph, or the chain when the module routes by destination), and
ICMP/uRPF (not the Router's business). A packet whose destination has no route and a packet whose neighbor is
unresolved are distinct, counted outcomes; the module drops both by default, and it can send either to a gate
instead (`punt_miss_gate`, `punt_unresolved_gate`, D-162) -- the frame is counted either way, so the counters
stay the record of what happened. A packet to a local route is a third outcome: it is delivered, not dropped,
and `punt_local_gate` says where the graph's local path is (D-178); dropping is the default because a graph
that names no local path has nowhere to deliver, and the `local` counter still records the traffic.

## Domains are cheap: `max_domains` is a bound

`max_domains` bounds the domain ids; it does not allocate them. A domain's FIB is created when it is first
needed — explicitly (`CreateDomain`), or by the first route a transaction writes into it (D-158). A router
configured for four network instances pays for the instances that hold routes, not for the configuration.
That first write pays the table's build: `rte_lpm_create`/`rte_fib_create` (and, for IPv6, `rte_fib6`'s
first instance) runs synchronously, under the engine's lock and the router's, sized by the module's
`max_ipv4_routes`/`max_ipv4_tbl8s` and `max_ipv6_routes`/`max_ipv6_tbl8s`, so one transaction can block control
operations for the length of that create — the cost table's rebuild column prices create plus every add at a
given size, an upper bound for it (`docs/dataplane-tables.md`); later writes are ordinary live updates.
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

- `docs/neighbor-resolution.md` — the resolver: state machine, identity, publication and what Vif keeps.
- `docs/dataplane-tables.md` — the tables themselves, their capacities and their update costs.
- `docs/decisions.md` — D-046 (domains), D-111 (the LPM resource), D-132 (the FIB backends), D-153 (address
  writes), D-155 (routing by destination), D-156 (groups), D-157 (the shared chain), D-158 (lazy domains),
  D-160 (a hop that forwards nowhere), D-175 (IPv4 neighbor resolution), D-176 (IPv6 ND on the same resolver),
  D-178 (connected/local route derivation and local delivery), D-180 (IPv6 routing, families, the Router's own
  neighbor resolution).
- `examples/appliances/router/router_app.h` — R1, the reference application, and `examples/static_router` for
  the same code as a program without `bessd`.
