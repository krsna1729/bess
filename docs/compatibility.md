# Compatibility

What a BESS release promises, and how each promise is checked. The details
live in the documents linked; this page is the summary a user of a release
reads (roadmap M26).

## Wire protocol (gRPC / protobuf)

The protobuf API (`protobuf/`, both the legacy `BESSControl` service and the
generic `bess.pb.v2.Control`) is the primary compatibility boundary.

- Changes are wire- and JSON-compatible: `buf breaking` (rule set
  `WIRE_JSON`, `buf.yaml`) runs on every pull request against the target
  branch (CI job "Protobuf compatibility", a required check on `develop`).
  A deliberate break is declared with the `buf skip breaking` label and a
  decision record.
- Removed meanings keep their numbers: a value that no longer applies stays
  defined and documented as never sent (for example
  `TransactionRecord.VISIBILITY_ATOMIC`), so old clients still decode.
- Clients in other languages can implement the protocol from the `.proto`
  files alone; `pybess.sdk` is a convenience and correctness layer over it
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
- A deprecated API keeps working for at least one release after the release
  that announces its replacement, and its removal is a decision record.
  Protobuf fields and enum values are never reused (above).

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
