# Reference appliances (roadmap M24)

Small but realistic applications that prove BESS is mechanism-oriented: each
is written against the installed `bess-dev` headers, owns its programming
model, and uses BESS only for packet execution, state, lifetime and
publication. Each is built as a plugin; its application code also runs with no
module graph through the module's `self_test` command.

| appliance | application-owned | BESS batteries | proves |
|---|---|---|---|
| R1 `router/` | static policy, interface-to-gate mapping, flow hash, unresolved-neighbor policy | route domains, next hops and groups, neighbor table, checked parse, TTL | overlapping VRFs, ECMP that keeps a flow on one path, a neighbor change with no route written, direct and graph paths |
| R3 `vswitch/` | Layer, Group, Rule, CompiledDecision; tenants as policy scopes; the layer semantics | decision cache and generation invalidation, RCU-published immutable policy, checked parse | BESS knows none of the policy model; a group switch atomic per tenant; invalidation O(1) by generation (no cache walk), other tenants keep their hits; no graph needed |
| R2 `nat/` | pool policy, firewall-then-NAT order, the clock | checked parse, conntrack, NAT bindings (worker-owned, reverse alias), port pool, expiry | the reply through the reverse alias, an outside-started connection refused after translation, idle mappings ended on time, nothing NAT-specific in the runtime |

Build and check against a staged install (as CI's install check does):

```
meson setup build examples/appliances    # PKG_CONFIG_PATH names bess-dev.pc
meson compile -C build
python3 tools/check_standalone_plugins.py --bessd <bessd> --plugin-dir build --set appliances
```
