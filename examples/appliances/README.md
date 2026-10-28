# Reference appliances (roadmap M24)

Small but realistic applications that prove BESS is mechanism-oriented: each
is written against the installed `bess-dev` headers, owns its programming
model, and uses BESS only for packet execution, state, lifetime and
publication. Each is built as a plugin; its application code also runs with no
module graph through the module's `self_test` command.

| appliance | application-owned | BESS batteries | proves |
|---|---|---|---|
| R1 `router/` | static policy, interface-to-gate mapping, flow hash, unresolved-neighbor policy | route domains, next hops and groups, neighbor table, checked parse, TTL | overlapping VRFs, ECMP that keeps a flow on one path, a neighbor change with no route written, direct and graph paths |
| R3 `vswitch/` | Layer, Group, Rule, CompiledDecision; tenant roots and revision-tagged compiled decisions | ScopeTable/ScopeResource, worker flow tables, RCU, checked parse | a real wire-controlled scope snapshot; one bound root drives marking and egress; cached decisions cannot cross policy versions; other tenants keep their own policy |
| R5 (unenrolled mode of R3) | hotness and promotion/revocation hooks; version-tagged decisions | flow tables for cached decisions and cold counts, hardware flow-rule owner | cold flows stay uncached; hot decisions can be promoted; a direct policy switch revokes hardware copies; enrollment refuses this mode because generic scope publication cannot revoke those copies |
| R4 `session/` | Session, PdrLikeRule, SessionAction, QosPolicy; their wire types (`session_appliance.proto`) and codecs | slot tables as transactional resources, the engine's reference checks across owners, the router, meters, GTP-U header | one transaction installs a session across BESS's router and the application's resources over the control API; a session naming a missing next hop is refused whole; a next hop an action names cannot leave |
| R2 `nat/` | pool policy, firewall-then-NAT order, the clock | checked parse, conntrack, NAT bindings (worker-owned, reverse alias), port pool, expiry | the reply through the reverse alias, an outside-started connection refused after translation, idle mappings ended on time, nothing NAT-specific in the runtime |

R4 (D-094) registers its three resources and its router with bessd's engine
(`init_context().resources()`) and binds a codec to each of the five
(`init_context().codecs()`): its own messages for QoS policies, actions and
rules, BESS's `Router*` messages for next hops and routes. The build compiles
`session/session_appliance.proto` with `protoc` and installs its descriptor set
(`session_appliance.desc`) next to the plugin; the check loads it to build
the messages and drives the resources with `ApplyTransaction`. Its
`self_test` decodes one session through the same codecs, writes it into a
private application and runs the fused path on frames.

## Tenant policy replacement (R3, D-197)

`VswitchAppliance` registers `<module>/policies`. The key is
`vswitch_appliance.TenantKey`: id 1 names input tenant 0, id 2 names tenant 1.
The value is `vswitch_appliance.PolicyValue`, the complete ordered list of
layers, groups and rules. The build installs `vswitch_appliance.desc`;
`GetSchema` provides the same types to a dynamic controller.

Upsert one complete value with `ApplyTransaction` consistency
`CONSISTENCY_SCOPE_SNAPSHOT` (C++ SDK: `NewTransaction({.snapshot = true})`).
For example, this value marks DSCP 20 and selects gate 1 as one policy:

```json
{
  "layers": [
    {"groups": [{"rules": [{"action": "MARK", "dscp": 20}]}]},
    {"groups": [{"rules": [{"action": "REDIRECT", "gate": 1}]}]}
  ]
}
```

Rules match a canonical IPv4 destination/prefix, destination-port range and
IP protocol. An absent upper port bound means 65535; protocol 0 means any.
The first matching group/rule decides a layer. Deny wins; otherwise marks
and redirects accumulate, with the last layer winning. An empty layer has
no effect. The codec rejects unknown actions, malformed prefixes, reversed
or oversized port ranges, protocols above 255, DSCP above 63 and gates
outside 0-1 before publication. Neither revision numbers nor cached decisions
are client fields.

Each packet binds one immutable root. Its compiled decision is usable only
for that root's revision. The old two-step policy/generation publication is
gone. Both tenants start with the reference policy; an erased scope drops
after the framework's removal cascade. Counters and cold-flow state remain
outside immutable policy. Two scopes do not switch all at once, and this
software guarantee does not cover hardware copies. R5 retains its direct,
unenrolled switch/revocation path; it cannot be enabled on an enrolled R3.

The installed-tree check `StandalonePlugins.AppliancesScopeSnapshot` updates
two-layer policies while a worker runs. Gate-local classifiers reject mixed
DSCP/egress pairs. It also checks rejected-update readback, tenant isolation
and replacement of a cached allow with deny.


Build and check against a staged install (as CI's install check does):

```
meson setup build examples/appliances    # PKG_CONFIG_PATH names bess-dev.pc
meson compile -C build
BESSD=<bessd> BESS_PLUGIN_DIR=build \
  <bess build>/tests/live/live_standalone_plugins --gtest_filter='StandalonePlugins.Appliances*'
```

R1's application also runs as a program without bessd, linked from the
installed archives (`examples/static_router`, D-095):

```
meson setup build-static examples/static_router   # PKG_CONFIG_PATH names bess-dev-static.pc
meson test -C build-static
```
