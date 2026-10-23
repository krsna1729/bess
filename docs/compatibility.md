# Compatibility

What a BESS build promises, and how each promise is checked. The details
live in the documents linked; this page is the summary a user of a build
reads (roadmap M26).

## Pre-1.0 policy

No BESS release has shipped (no tag, no published artifact). The installed
C++ headers keep their public/experimental/internal classification and the
plugin ABI checks below; the protobuf surface has no compatibility promise
(D-173).

## Wire protocol (gRPC / protobuf)

The protobuf API (`protobuf/`) is the primary control boundary: the generic
`bess.pb.v2.Control` service. The legacy `BESSControl` service is gone
(D-140), so its methods are no longer served.

- The surface may change without compatibility guarantees (D-173). A field,
  enum value or message that no longer applies is deleted outright: its
  number and name are not reserved, and no deprecation window is promised.
- `buf breaking` (`WIRE_JSON`, `buf.yaml`, CI job "Protobuf compatibility")
  runs and is advisory until a 1.0 compatibility policy exists. It is not a
  required check, and a reported break does not block a pull request. No
  `buf skip breaking` label is used.
- Clients in other languages implement the protocol from the `.proto` files
  of the revision they target; the Go SDK (`sdk/go`) and the C++ SDK
  (`sdk/cpp`) are convenience and correctness layers over it
  (docs/control-sdk.md), not a second protocol.

## C++ source API

Installed headers are classified in `tools/api_classes.json`
(docs/architecture.md section 5, docs/plugin-api.md):

- **public**: supported source API for compatible releases. Plugins are
  rebuilt against the target release; the C++ ABI is not promised.
- **experimental**: installed and usable, may change without preserving
  source compatibility. Promotion to public is a decision record, after a
  reference appliance uses it and its API, generated code and lifetime
  contract are reviewed.
- **internal**: never installed. `tools/check_installed_headers.py` fails if
  an internal header is installed or a public header includes one, and
  compiles a plugin against the staged install to prove it.

## Plugins

A plugin carries a descriptor (`core/framework/plugin.h`):
`BESS_PLUGIN_ABI_VERSION` must match exactly; the daemon's
`BESS_PLUGIN_API_VERSION` must lie in the plugin's `[api_min, api_max]`; and
every `BESS_CAP_*` the plugin requires must be provided. A plugin that fails
any of these is refused at load, with the reason, never loaded and left to
crash. CI builds the standalone example plugins from the staged install only
and checks each exports its descriptor; the sample plugin is loaded into a
running daemon in the test suite.

## Experimental and deprecated

- Experimental headers and options say so in `tools/api_classes.json` and in
  their decision records; nothing experimental is a dependency of a public
  header.
- A deprecated C++ API keeps working for at least one release after the
  release that announces its replacement, and its removal is a decision
  record. Protobuf has no such window before 1.0 (above).

## Recorded breaks

- **`IPLookup` is retired** (D-161). The module is gone from the build and
  from the API: a manifest that names `mclass: IPLookup` no longer applies,
  the C++ class is removed, and `IPLookupArg`, `IPLookupRouteKey` and
  `IPLookupRouteValue` are deleted from `protobuf/module_msg.proto`.
  No release ever shipped them, and before 1.0 a deleted name is removed
  outright rather than reserved (D-173); this entry is the record. The
  replacement is the `Router` (D-155): the same route table (LPM or FIB),
  routes that name a next hop or, with `max_groups`, a group (D-156), and
  next hops that carry link addresses. `ROUTE_MODE_DESTINATION` routes a
  packet by its own IPv4 destination, what `IPLookup` did:
  `bessctl/conf/samples/router.yaml` is that shape, and
  `bessctl/conf/samples/vrf_routing.yaml` (one Router, one FIB per route
  domain, a Vif writing the domain) is the VRF shape the two IPLookups in
  that sample used to have.
- **`RouterArg` sizes each address family on its own, in destination mode
  only** (D-183). `max_routes` and `max_tbl8s` are removed:
  `max_ipv4_routes`/`max_ipv4_tbl8s` and `max_ipv6_routes`/`max_ipv6_tbl8s`
  replace them. In `ROUTE_MODE_DESTINATION` a family is enabled exactly when
  its route capacity is set, and a Router with neither is refused. In
  `ROUTE_MODE_NEXT_HOP_ID` (the default) the Router has no FIB: it forwards
  IPv4 and IPv6, refuses the four capacity fields, a non-default `backend`
  or `flow_hash`, a nonzero `max_groups`, `max_domains` above 1,
  `domain_attr`, `punt_miss_gate` and `punt_unresolved_gate`, and refuses
  every route and group write, so a next-hop-id configuration drops those
  fields, its routes and its groups (an empty `RouterArg` is valid).
  `max_routes` (field 1) and `max_tbl8s` (field 2) are deleted and their
  numbers left unused; every other `RouterArg` field keeps the number
  `develop` released (3-13), and the new fields take 14-27, so an older
  binary `RouterArg` is never misread. Text and YAML that name fields only
  need the capacity rename. An IPv4
  destination-mode Router that relied on the old default writes
  `max_ipv4_routes: 4096`.
- **Transactions reserve in resource rank order** (D-183). The engine calls
  `Resource::Reserve` for every operation on a referenced resource before the
  operations on the resources that reference it, and in request order within
  a resource; results are still reported in request order. A `Reserve` that
  reads another resource's staged state therefore sees every operation on
  lower-rank resources, wherever the request lists them.

## What a build is made of

Every install carries `share/bess/build-info.json` (version, commit, plugin
API version, compilers, C++ standard, ISA and build options, the DPDK pin
with its source checksum, and every dependency's version) and an SPDX 2.3
SBOM, `share/doc/bess/bess.spdx.json`, generated from the same facts by
`tools/build_info.py`. CI's install check requires both.

## Hardware validation

Software completeness and hardware validation are tracked separately: no
release claims hardware behaviour that was not run on that hardware. CI
validates the software against the unix-socket and pcap ports, DPDK's
`net_null` device, and fakes for the hardware seams
(`offload/fake_flow_backend.h`); no physical NIC is in the loop.

| Area | Status | Where |
|---|---|---|
| Physical NIC over VFIO, MTU/jumbo, scatter RX | not validated (no NIC) | MODERNIZATION.md section 5 (C-HW) |
| Multi-queue RSS; symmetric RSS key and RETA pinning | configuration code not exercised (CI's ports have one receive queue; only the symmetric-inputs check ran) | D-080 |
| Real-NIC throughput baseline | not measured | C-HW |
| AF_XDP zero-copy | not validated (copy mode only) | C-HW |
| `MBUF_FAST_FREE`, MT-lockfree Tx, PortOut lock elision | not enabled; needs a PMD that advertises it and a workload | C-HW |
| Checksum offload on transmit | negotiated per port and reported (`GetCapabilities` `PortInfo`); real-NIC correctness not validated | MODERNIZATION.md K4.4b, D-082 |
| `rte_flow` offload (`offload/rte_flow_backend.h`) | probed against the null PMD only; lifecycle tested over the fake device | D-070 |
| Hardware meter backend | not implemented | C-HW |
| Certification matrix (Intel, NVIDIA/Mellanox, virtio, representors) | not run | D-070 |

A row changes only with a decision record that names the device, firmware,
DPDK version and the result.
