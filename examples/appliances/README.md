# Reference appliances (roadmap M24)

Small but realistic applications that prove BESS is mechanism-oriented: each
is written against the installed `bess-dev` headers, owns its programming
model, and uses BESS only for packet execution, state, lifetime and
publication. Each is built as a plugin; its application code also runs with no
module graph through the module's `self_test` command.

| appliance | application-owned | BESS batteries | proves |
|---|---|---|---|
| R1 `router/` | static policy, interface-to-gate mapping, flow hash, unresolved-neighbor policy | route domains, next hops and groups, neighbor table, checked parse, TTL | overlapping VRFs, ECMP that keeps a flow on one path, a neighbor change with no route written, direct and graph paths |

Build and check against a staged install (as CI's install check does):

```
meson setup build examples/appliances    # PKG_CONFIG_PATH names bess-dev.pc
meson compile -C build
python3 tools/check_standalone_plugins.py --bessd <bessd> --plugin-dir build --set appliances
```
