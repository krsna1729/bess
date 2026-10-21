# Neighbor resolution

`core/route/neighbor_resolver.h` is the IPv4/Ethernet neighbor resolution
library (M15, D-175): it solicits with ARP, learns replies, retries with
backoff, ages resolved neighbors, follows a MAC change, publishes failure and
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
to reject) and had no IPv6 ND path. ARP replies arriving from the network were
just demultiplexed like any other frame.

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
`(InterfaceId, IPv4 address)`:

| event | call | effect |
|---|---|---|
| create | `Bind(hop, key, now)` | binds a next hop through `NeighborTable`, starts resolution if the neighbor is new |
| learn | `OnArp(now, ingress, frame)` | a valid reply resolves the neighbor and publishes its new MAC |
| refresh | `OnArp` again, or `Poll` when the aging timer fires | re-arms the lifetime, or starts a fresh solicitation |
| fail | `Poll` after the last probe | publishes `kUnreachable` |
| recover | `Poll` after the retry interval | publishes `kIncomplete`, solicits again |
| identity change | `ConfigureInterface` / `RemoveInterface` | cancels timers, invalidates stale replies, republishes |

The state machine is:

```text
            Bind                      reply
   (new) --------> kIncomplete ----------------> kResolved
                       |  ^                          |
             max probes|  | retry/aging               | lifetime
                       v  |                          v
                  kUnreachable -----------------> kIncomplete
```

`Poll(now, budget)` returns the solicitations to send and the next-hop updates
to publish; `OnArp` returns the updates for a reply. The caller owns packet
buffers, gates and the policy for unresolved traffic. That keeps the library
free of `Module`, gate, worker and protobuf dependencies, as the rest of
`core/route` is.

Interface identity is explicit: `ConfigureInterface({id, mac, ipv4})` assigns a
generation (epoch) per interface. Every entry records the epoch it was created
under, and a reply is accepted only if the arrival interface, the epoch, the
reply's sender and target addresses, its Ethernet source/destination and a
unicast sender MAC all agree with the current identity. A reply in flight when
an address moves is therefore a stale record, not an update to a newer
neighbor. A reply never creates an entry: only `Bind` does, so unsolicited
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
handled, never ignored: `OnArp` leaves the entry, its timer and the table
untouched and ignores the reply, `Bind` rolls back, and an entry whose `Poll`
or `ConfigureInterface` transition cannot be armed is dropped rather than kept
without a timer (its committed state is incomplete/unreachable, so the hop
does not forward).

## Publication and the packet path

The resolver never writes a `NextHop` in place. It asks `NeighborTable::Update`
for the new immutable objects and returns them:

- direct users call `Publish(router, updates)`, which is one
  `Router::SetNextHop` per bound hop: one pointer store each, no FIB write;
- transactional users build `NextHopOps(router, updates)` and apply them in
  one `TransactionEngine::Apply`: all-or-nothing (a refused transaction makes
  none of them visible), in the engine's referential order -- each next-hop
  object is replaced whole, one at a time, so a packet may see one hop's new
  adjacency and another's old one before the transaction finishes. This is
  not a snapshot: the Router's next-hop resource is referential, and a
  `kScopeSnapshot` request is refused.

In both cases a packet-path lookup (`Router::Resolve` /
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
carried before rethrowing (the wheel had already spent those nodes). `OnArp`
stages its update and output space and commits the resolved state only once
its aging timer is in place, so a timer refusal leaves the entry and its timer
as they were. A refused call therefore leaves the neighbor table, the
published adjacency and the timers exactly as they were, and the same call
retried converges.
`NeighborResolverFaultTest` fails every allocation point of Poll, Configure and
Remove in turn and checks that invariant.

The resolver itself is single-threaded (one control-side writer). ARP frames
arriving on workers must be handed to that writer; the packet path never calls
into the resolver. This is the same ownership split as every other control-side
table in BESS and is what lets the state machine allocate without locks.

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

Not implemented. The state machine, identity check, publication path and timer
ownership are address-family neutral; ND is another packet family over the
same entries (solicitation/advertisement helpers, a hop-limit/multicast MAC
mapping, and an `InterfaceId`-scoped link-local address) rather than another
resolver. The key is currently IPv4, as the FIB is.
