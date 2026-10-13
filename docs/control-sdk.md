# Control SDKs (`pybess.sdk`, Go `sdk/go`, C++ `sdk/cpp`, M27)

A thin client over the generic control API (`protobuf/control_v2.proto`). It
adds no semantics of its own: resources are whatever modules registered
(`<module>/<table>`), keys and values their own protobuf messages. What it
adds is the part of the protocol that is easy to get wrong when every
controller writes it again.

```python
from pybess import sdk

client = sdk.Client('localhost:10514')
rules = client.resource('em0/rules')          # discovered, with its types
with client.transaction(expected_generation=g) as tx:
    tx.upsert(rules, key, value)              # types checked here
    tx.erase(rules, other_key)
print(tx.result.generation)                   # committed on a clean exit
```

Go (`github.com/krsna1729/bess/sdk/go/bess`), the same guarantees with Go
errors (`*ConflictError`, `*RejectedError`, `*BusyError`, `*TransportError`,
`*DaemonRestartedError`, `*StaleResourceError`, `*InvalidRequestError`):

```go
client, err := bess.Dial("localhost:10514")
rules, err := client.Resource(ctx, "em0/rules")
tx := client.Transaction(bess.WithExpectedGeneration(g))
err = tx.Upsert(rules, key, value)     // any proto.Message, types checked
applied, err := tx.Commit(ctx)
```

The Go module carries its generated `control_v2` code (`sdk/go/controlv2`),
made by `tools/gen_go_sdk.sh` in a pinned container; CI regenerates it and
fails on any difference, so it never drifts from the `.proto`. Keys and values
of module resources need no generated Go types: any `proto.Message` works,
including `dynamicpb` messages built from a descriptor set.

C++ (`sdk/cpp`, `#include <bess/sdk/client.h>`, pkg-config `bess-sdk`, C++23),
the same guarantees with `std::expected` results whose `Error::kind` names the
outcome (`kConflict`, `kRejected`, `kBusy`, `kTransport`, `kDaemonRestarted`,
`kStaleResource`, `kInvalidRequest`, `kPipelineConflict`):

```cpp
auto client = bess::sdk::Client::Dial("localhost:10514");
auto rules = client->Resource("em0/rules");
auto tx = client->NewTransaction({.expected_generation = g});
tx.Upsert(*rules, key, value);         // any protobuf Message, types checked
auto applied = tx.Commit();            // Result<Applied>
```

`libbess-sdk` carries the generated `control_v2` code and installs
`control_v2.pb.h` next to its headers; it depends on gRPC++ and protobuf, not
on bessd. Release builds ship it as the `bess-sdk` .deb (a static archive, its
headers and `bess-sdk.pc`), which needs no `bess` package. Keys and values may
be `DynamicMessage`s built from a descriptor set. `WatchEvents` blocks and
calls back per event; stop it with a `std::stop_token`. A `Transaction` refers
to the `Client` that made it, which must outlive it. The Go SDK is the one for
external controllers such as the UPF; the C++ SDK is the one the C++ bessctl
(G1.7, pending) is to be built on (D-104).

## Desired state

The pipeline (ports, modules, connections, workers, traffic classes, and where
each module's task is scheduled) is desired state: `PipelineBuilder`
assembles a `control_v2.Pipeline`, empty or from `client.pipeline()`'s
snapshot, and the daemon validates, diffs, plans and applies it. The SDK
repeats none of the planner's logic.

A task placement (`task('src', parent='limit')` or `task('src', wid=1)`;
Go and C++ `Task`) puts a module's task under a traffic class or at a
worker's root, what `attach_task` did. A task no placement names stays where
it is; a new one goes to any worker. Placements apply together, so tasks may
trade places (D-109).

```python
snap = client.pipeline()                       # PipelineSnapshot: pipeline, version
p = (sdk.PipelineBuilder(snap.pipeline)
     .module('em', 'ExactMatch', module_msg.ExactMatchArg(...))
     .chain('rnd', 'em')
     .build())
steps, version = client.plan_pipeline(p)       # what the daemon would do
client.apply_pipeline(p, expected=snap.version)
```

Go: `bess.NewPipeline(snap.Pipeline).Module(...).Chain(...).Build()`,
`client.Pipeline`, `ValidatePipeline`, `DiffPipeline`, `PlanPipeline`,
`ApplyPipeline(ctx, p, bess.WithExpectedVersion(snap.Version))`, with
`*PipelineConflictError`. C++: `client->ApplyPipeline(p, snap->version)`.

A version is a generation and the daemon epoch it belongs to
(`PipelineVersion`): generations start again when the daemon restarts, so a
generation alone means nothing across a restart (D-106).

`apply_pipeline` retries a busy answer within the retry policy and never a
conflict (`PipelineConflict`, a `Conflict`) or a refusal. It carries no request
id, so after no answer the outcome is unknown (`TransportError`): read
`client.pipeline()` before applying again, or apply with `expected` so a
second application is a conflict. An answer is told from a loss by the
server's `ErrorDetail` (a resource failure is UNAVAILABLE with a detail: an
answer).

## Schema discovery

`GetSchema` (C++ `client->Schema()`, Go `client.Schema(ctx)`) names the
argument type of every module class's init, every port driver's init and every
module command, by full protobuf name, and carries a `FileDescriptorSet` with
those types and transaction resources' key and value types, each file after
its imports. A client without generated code for a type (bessctl, a manifest,
a UI) builds a `DescriptorPool` from it and reads or writes the type as JSON
or as a `DynamicMessage`. Loaded plugins' types are included. A class
registered with `DEF_MODULE`/`ADD_MODULE` or `ADD_DRIVER` declares its init
type from `Init`'s parameter; a command's hand-written `arg_type` resolves in
the init type's package, then `bess.pb` (`""` if neither names a message; a
unit test holds every built-in command to a real type).

## bessctl (C++, G1.7)

bessctl's code is built on the C++ SDK alone and ships inside bessd (D-120):
`bin/bessctl` (in the `bess` .deb; `build/bessctl/bessctl` in a build
directory) is a symlink to `bessd`, and `bessd ctl ARGS` is the same command.
`build/bessctl/bessctl-standalone` is the client alone, on `libbess-sdk`, for
a host without bessd; it is not installed. Pipelines are manifests: a control_v2 `Pipeline` in
protobuf's JSON mapping, written as JSON or YAML, field names as in
`control_v2.proto`. A module's or port's `arg`
may leave out `"@type"`; bessctl takes it from `GetSchema` (the class's or
driver's init type), and `bessctl get` leaves it out where it is that type, so
what `get` prints, `apply` reads back. Numbers are passed through as written:
64-bit values are not rounded.

```sh
bessctl schema ExactMatch         # init type, gates, commands and their types
bessctl validate pipeline.json    # the daemon's canonical form
bessctl diff pipeline.json        # what would change
bessctl plan pipeline.json        # the steps, by phase
bessctl apply pipeline.json [--generation G@E]
bessctl get > pipeline.json       # the active pipeline as a manifest
bessctl get -o yaml > p.yaml      # the same, as YAML (also validate -o yaml)
bessctl status | resources | metrics | events [--from SEQ] [TYPE...]
```

A file is YAML if it ends in `.yaml`/`.yml`, JSON if `.json`, else JSON when it
starts with `{` (so `-`, standard input, takes either). YAML reads into the
same tree as JSON (D-108): plain scalars resolve as YAML 1.2's core schema
does (`null`/`~`, `true`/`false`, decimal numbers as written, `0x`/`0o` in
decimal, `.inf`/`.nan` as protobuf's `"Infinity"`/`"NaN"`, else a string);
quoted scalars and `!!str` are strings; anchors and aliases work, within a
bound on what the document expands to (256 MiB, 256 levels). Other tags,
merge keys (`<<`), duplicate keys and more than one document are refused,
with the line and column.

```yaml
modules:
  - {name: src, mclass: Source, arg: {pkt_size: 128}}
  - name: em
    mclass: ExactMatch
    arg:
      fields: [{offset: 23, num_bytes: 1}]
  - {name: sink, mclass: Sink}
connections:
  - {upstream: src, downstream: em}
  - {upstream: em, downstream: sink}
resources:
  em/rules:
    - key: {fields: [{value_bin: "EQ=="}]}
      value: {gate: 0}
```

A manifest is the whole desired state: workers and traffic classes included
(start from `bessctl get`). The daemon validates; bessctl only converts.

A worker's `core` may be left out (D-123): the daemon then picks a free CPU
of its memory node -- one per physical core before any core's second thread,
never the reserved control CPU, never a CPU another worker holds -- and keeps
it for the worker's life. `bessctl get` and `GetPipeline` show the CPU it
runs on (and `GetPipelineResponse.workers` its node, `cross_numa`,
`shares_core`, `on_control_cpu`); applying the same manifest again, core still
left out, moves nothing. No free CPU is `RESOURCE_EXHAUSTED` (ENOSPC, field
`core` in the detail): an answer, refused, never retried as no answer is. An
explicit `core` is honoured anywhere in bessd's CPU set, with a warning when it
is on another node, the control CPU, a CPU another worker uses, or a sibling
of one; outside the set it is refused. In the SDKs: C++
`Worker(wid, std::nullopt)`, Go `AutoWorker(wid, scheduler)`, pybess
`worker(wid)` (`core=None`). `GetCapabilities` carries the daemon's
`memory_node` and its one-line `placement` summary (`bessctl status` prints
both); `GetPortStatus` a port's `numa_node` (-1: unknown) and `cross_numa`.
`GetCapabilities` also carries the SIMD width DPDK and BESS kernels use
(D-126): `simd_width`, BESS kernels' width in bits, `dpdk_simd_width`, DPDK's own limit after EAL init (32768: unlimited; equal on x86, DPDK's default on arm64), and the `simd` summary line (`bessctl status`
prints it; see [placement.md](placement.md#simd-width)).

A left-out `core` no longer means CPU 0. The field gained presence, so the
bytes are unchanged but their meaning is not: a client built from the proto
before D-123 sends nothing for `core: 0` (proto3's default), which a D-123
daemon reads as automatic. Explicit core 0 needs a client built from the
current proto (every SDK in this repository is).

A manifest may also hold entries for the resources its modules create (see
`bessctl resources`), in the resource's key and value types:

```json
"resources": {
  "em/rules": [{"key": {"fields": [{"value_bin": "CgAAAQ=="}]}, "value": {"gate": 1}}]
}
```

`apply` commits them in one transaction after the pipeline, since the
resources exist only then: if they fail, the pipeline stays applied and the
error names the entry. Both steps are idempotent, so after a fix the same
`apply` is the retry; with `--generation`, give the version the failed
`apply` printed (the error says which), since its pipeline step moved it.
Versions are printed and read as `generation@epoch`: a generation alone
would match a restarted daemon that reached the same number (D-106).
Entries are added or replaced, never removed, and `get` does not print them.
`validate`, `diff` and `plan` check only the pipeline.

## Module commands (D-115)

`RunModuleCommand` (Control service; C++ `client->RunModuleCommand(module,
command, arg)`, Go `client.RunModuleCommand(ctx, module, command, arg)`) runs a module's
command, as the legacy `ModuleCommand` did: `arg` is a message of the type
`GetSchema` names for the command, and the answer is a `google.protobuf.Any`,
empty when the command answers nothing. Every module also answers
`get_initial_arg` (`EmptyArg`): its init argument. A command is not a
transaction: it is not retried, recorded or announced as an event, and the
pipeline's generation does not change. It names the epoch the client has seen
and is refused under another (D-106). A command not marked thread-safe is
refused (EBUSY: C++ `kBusy`, Go `*BusyError`) while a running worker runs the module: pause the workers
first. An unknown module is `NOT_FOUND`; an unknown command, or the command's
own failure, carries its errno in the `ErrorDetail`.

```sh
bessctl command fib lookup '{addrs: ["02:00:00:00:00:01"]}'   # JSON or YAML
bessctl command em get_runtime_config -o yaml
bessctl pause && bessctl command arp add '{ip: 10.0.0.1, mac_addr: "02:00:00:00:00:01"}' && bessctl resume
```

## Administration (the Admin service, D-107)

Beside `Control`, bessd serves `bess.pb.v2.Admin`: what an operator does to a
running daemon besides its pipeline. Both SDKs wrap it (C++ / Go):

| Call | What it does |
|---|---|
| `ImportPlugin(path)` / `UnloadPlugin(path)` | load or unload a plugin (a path on the daemon's host) |
| `Ports(names...)` | link state and rx/tx counters; every port by name, or the named ones in that order |
| `PacketPools()` | the default packet pool (one, on the daemon's memory node, D-123): size, in use, available, its socket |
| `SetCapture(request)` | start or stop a pcap or pcapng capture on a module's input or output gates (one, or all; pcap without a gate needs the module to have only one there, since a pcap stream holds one file header) into a FIFO the caller made; at most one capture of each format per gate (`ALREADY_EXISTS`), all gates or none |
| `PauseWorkers()` / `ResumeWorkers()` | every worker; resuming runs the resume hooks first. A worker a pipeline creates starts paused, and applying a pipeline keeps the workers' state |
| `Shutdown()` | the daemon answers, then stops |

The mutating calls name the epoch the client has seen and are refused under
another, as `kDaemonRestarted` / `*DaemonRestartedError{Refused}`, with
nothing done (D-106); they are not retried.

```sh
bessctl apply pipeline.json && bessctl resume
bessctl ports | pools
bessctl capture em0 out:1 | tcpdump -n -r -     # Ctrl-C stops the capture
bessctl capture em0 in --pcapng > em0-in.pcapng
bessctl plugin import /opt/bess/plugins/my.so
bessctl pause | resume | shutdown
```

`capture` makes the FIFO under `/tmp`, so the daemon must share the host's
`/tmp`; it stops the capture when interrupted, when the stream ends, or when
its reader goes away.

Not yet: the legacy commands (daemon start, `.bess` files, interactive
shell); the Python bessctl remains for those until D-104's last step.

## HTTP (in bessd, D-125)

Off by default. `bessd --http_addr 127.0.0.1:8080` (or `BESSD_HTTP_ADDR`)
serves HTTP on its own threads, none of them a worker. The host is required:
a loopback host keeps it on the machine, and any other (`0.0.0.0`, a routable
address) serves the network with no authentication and no TLS, which bessd
logs as a warning. Each request is a call on bessd's own gRPC server through
an in-process channel, so HTTP and gRPC answer alike, under the same epoch
rules (D-106), and there is one authorization point.

| Route | What |
|---|---|
| `POST /v2/<Method>` | a unary `bess.pb.v2.Control` RPC: the body is the request in protobuf JSON (empty: `{}`), the answer the response (lowerCamelCase names, 64-bit integers as strings) |
| `GET /v2/events` | `WatchEvents` as server-sent events: one JSON `Event` per `data:` line, `id: EPOCH:SEQUENCE`; `?from_sequence=&daemon_epoch=&types=a,b`, or the `Last-Event-ID` a browser's `EventSource` sends on reconnecting |
| `GET /metrics` | `ListMetrics` in the Prometheus text format 0.0.4: HELP and TYPE before each family, families in name order, `counter`/`gauge` as declared, then `bess_daemon_epoch` |
| `GET /` | a static page, compiled in, listing modules and metrics and following events through the routes above |

A failed call answers the gRPC code's HTTP status (400 for INVALID_ARGUMENT
and FAILED_PRECONDITION, 404, 409 for ALREADY_EXISTS and ABORTED, 503 for
UNAVAILABLE, ...) with `{"code", "status", "message", "detail"}`, `detail`
being the typed `ErrorDetail`; a body that is not the request's JSON is 400.
One request per connection. The Admin service is not on HTTP.

```sh
curl -s localhost:8080/metrics
curl -s -X POST localhost:8080/v2/GetPipeline
curl -s -X POST localhost:8080/v2/ApplyPipeline \
  -d '{"pipeline": {"modules": [{"name": "s", "mclass": "Sink"}]}, "daemonEpoch": "EPOCH"}'
curl -sN localhost:8080/v2/events
```

## What they guarantee

| Situation | What the SDK does |
|---|---|
| One logical transaction | One `request_id` for its whole life, kept across every retry. |
| Before the first send | Learns the daemon epoch (resource discovery) if this client has not. |
| Every send | Names the epoch it was built under (D-106): a transaction its resources', a pipeline apply its expected version's, else the last one the client saw. |
| The daemon refuses the epoch (`EPOCH_MISMATCH`) | `DaemonRestarted` with `refused` set (C++ `kDaemonRestarted` with the detail; Go `Refused`): the daemon restarted since the request was built and applied nothing of it. An answer: not retried, nothing asked; the client now knows the new epoch. |
| No answer (deadline, unavailable; `INTERNAL`/`UNKNOWN` without BESS's error detail) | Asks `GetTransaction(request_id)`, again if the question goes unanswered too. Known: that outcome (`replayed`). Answered "not known" under the epoch the commit started with: sends the identical request again (the daemon replays or applies it once). Never sends again on a guess. |
| No answer through every attempt | `TransportError`: the outcome is unknown, not failed; ask `get_transaction(request_id)` later. |
| An answer to a question from another daemon epoch | `DaemonRestarted` (not `refused`): a send went unanswered and the old daemon's outcome is lost with it; never reported as applied or as not applied. The running daemon has not applied it (it refuses the old epoch): re-read its state before deciding. |
| `OUTCOME_BUSY` | The daemon has already waited out a short BUSY itself (up to 5 more attempts over about 6 ms, within the request's deadline; nothing is recorded under the id). The SDK then retries with doubling backoff, then `Busy`. Metrics: `bess_transactions_total{outcome="busy"}` counts only BUSY answers; attempts the daemon retried count in `bess_transaction_busy_retries_total`. |
| `OUTCOME_CONFLICT` | `Conflict` (nothing tried); the application decides what a moved generation means. `after_unknown_attempt`: an earlier send of the same commit went unanswered and may have applied. |
| `OUTCOME_REJECTED` | `Rejected`, with each failing operation's index and error; nothing changed. |
| A refused call | `InvalidRequest`, with the server's `ErrorDetail` (code, field, object). |
| A key or value of the wrong type | `InvalidRequest` before anything is sent. |
| A resource handle from before a restart the client has seen | `StaleResource` before anything is sent. A handle carries the epoch it was discovered under; a transaction is bound to its first handle's epoch. Look the resource up again (`client.resource(name)` after `resources(refresh=True)`, or after any answer from the new epoch) and rebuild the transaction. |

Conflicts and rejections are never retried. Tuning:
`sdk.RetryPolicy(attempt_timeout, attempts, busy_backoff)`. Every RPC counts
against `attempts`, status queries included: `attempts=2` is one send plus one
question. When the attempts run out after any unanswered send, the result is
`TransportError` (unknown), never `Busy`: an unanswered request may still be
waiting for the daemon and apply later; ask `get_transaction(request_id)`.

One limit: the daemon remembers outcomes in a bounded window (4096). A request
that applied and then aged out before the retry asked looks unseen and is sent
again. Where applying twice would matter, set `expected_generation`: the second
application is then a `Conflict`.
Consistency: `client.transaction(snapshot=True)` asks for
`CONSISTENCY_SCOPE_SNAPSHOT` (D-050); the default is referential.

Resource handles and restarts: the client knows of a restart only once the
daemon answers from the new epoch (any call: discovery, a commit, a status
question); that answer drops the resource cache and makes every older handle
stale. A restart the client has not seen yet passes these checks, but not the
daemon's: the request names the old epoch, and the restarted daemon refuses it
before any effect (D-106). An answer from another epoch to a question
(`GetTransaction` after an unanswered send) still raises `DaemonRestarted`
without `refused`, meaning "the outcome on the old daemon is lost": re-read the
state before deciding.

## Tests

`pybess/test_sdk_model.py` checks the commit protocol against every
small-scope behaviour (roadmap Appendix L, A1+): a simulated daemon keeps the
truth (its epoch, what it applied), and a depth-first search replays the
client against every combination of a lost request, a lost reply after the
daemon acted, a busy answer, and a daemon restart before any RPC, for 1 to 4
attempts (8 to 121 paths). Invariants: at most one application over all
epochs; `Applied` only if applied, in the answering epoch; `Busy` only if
nothing applied; `TransportError` only after an unanswered RPC;
`DaemonRestarted` only after a restart. A blind resend, an ignored epoch and
`Busy` after an unanswered send each fail it.

| Model action | Code |
|---|---|
| Send | `Client._commit` → `ApplyTransaction` |
| Ask | `Client.get_transaction` → `GetTransaction` |
| Resend | `_commit`, only after "not known" under the commit's epoch |
| Epoch check | `Client._check_epoch` / `_observe_epoch` |
| Out of attempts | the end of `_commit` (`TransportError` or `Busy`) |

The Go and C++ clients follow the same contract (`sdk/go/bess/client.go`,
`sdk/cpp/src/client.cc`); their scripted tests cover the same cases.

`pybess/test_sdk.py`, `sdk/go/bess/client_test.go` and
`sdk/cpp/client_test.cc` script every recovery path against a fake transport
(the `python` suite; the CI job "Go SDK", with the race detector; the `sdk`
suite). Against a live daemon (the `live` suite, `tests/live/`):
`control_sdk_test.cc` (C++) and the conformance suite.

Black-box recovery against the live daemon (`control_sdk_test.cc`, the real
transport with ApplyTransaction's fate scripted): an answer lost after the
daemon applied is recovered through GetTransaction (`replayed`, the daemon's
applied counter moves by one, the rule steers); a request lost before it
arrived is sent again once "not known"; a daemon restarted in place between
the send and the question is `kDaemonRestarted`, the old resource handle is
then `kStaleResource` and never sent, and a pipeline built under the old epoch
is refused. A blind resend after a timeout, or an ignored epoch, fails them.
Whether a rule steers is read from the gate counters (D-116) of a Source ->
Update -> ExactMatch pipeline (`tests/live/steering.h`).

A real controller on the SDK: `tools/live_transaction_bench.py` (D-027's live
gate) applies its pipeline as desired state and drives its rules as SDK
transactions; it reads BUSY answers from the daemon's metrics.

### Conformance suite

`sdk/conformance/cases.json` is run, case by case, by each SDK's driver against
one live bessd (D-104), so the SDKs cannot drift: `sdk/cpp/conformance.cc`
(`sdk_cpp_conformance`) and `sdk/go/bess/conformance_test.go`
(`go test -tags live -run TestConformance`, one subtest per case).
`tests/live/conformance_test.cc` creates an ExactMatch, runs both drivers
(`$BESS_GO` or `go`; `BESS_SKIP_GO=1` skips Go), requires every case to pass
in each, and checks that the rule each left behind steers packets. A behaviour
both SDKs must share is a case there, not a test in one SDK.

A driver gets the daemon's address, the resource name (`$resource` in the
cases; `$module` is its module, the part before `/`) and the cases. It builds every message from the daemon's `GetSchema`
descriptors and protobuf JSON, as a client without generated code would.
Cases run in order, each with its own labels; a case fails at its first failing
step. Error kinds are named alike in both drivers: `invalid_request`,
`conflict`, `rejected`, `busy`, `transport`, `pipeline_conflict`,
`daemon_restarted`, `stale_resource`, `cancelled`. Every step has `"do"`; a
step with `"want_error"` passes only if it fails with that kind, otherwise only
if it succeeds.

| `do` | Fields | Checks |
|---|---|---|
| `capabilities` | `want_rpcs`, `want_classes`, `want_resources` | a daemon version, at least one module class, a plugin API version and a daemon epoch; these RPCs (full names), module classes and resources among those offered |
| `supports` | `rpc`, `want` | `Supports(rpc) == want` |
| `metrics` | | at least one sample |
| `schema` | `class` | the class's init type is in the schema's descriptors |
| `resource` | `name` | the resource exists (or `want_error`) |
| `watch` | `types`, `as` | opens an event stream from sequence 1 (what the daemon holds, then live), so no event of the case is missed |
| `event` | `watch`, `of`, `want_fields` | within 5 s the stream delivers commit `of`'s event (its `request_id` field), with that commit's generation and these fields |
| `commit` | `ops`, `request_id_of`, `expected_generation_of`, `generation_delta`, `snapshot`, `as` | one transaction on `$resource` (below) |
| `transaction_status` | `of` or `request_id`, `want_known`, `want_outcome` | `GetTransaction` of commit `of`'s request id, or `request_id` |
| `pipeline` | `base`, `add`, `chain`, `remove`, `plan`, `want_plan_version_of`, `want_changes`, `at_version_of`, `as` | below |
| `module_command` | `module`, `command`, `arg`, `class` or `arg_type`, `want_answer` | `RunModuleCommand` (D-115) with `arg` (protobuf JSON) in `arg_type`, else in the argument type `class`'s schema gives the command; the answer, in protobuf JSON with the .proto field names, has the fields of `want_answer` with those values |

**commit.** Each op is `{"op": "upsert", "key": {...}, "value": {...}}` or
`{"op": "erase", "key": {...}}` in the resource's key and value types;
`"key_as_value": true` builds the key in the value type instead. An op with
`want_error` must fail when added (the client refuses it before sending) and is
not part of the transaction. Options: commit `request_id_of`'s request id;
an expected generation of commit `expected_generation_of`'s plus
`generation_delta`; `snapshot` (scope) consistency. Checks: `want_replayed`,
`want_generation_of` (the same generation as that commit), `want_visibility`
(a `TransactionRecord.Visibility` name) and, for `rejected`,
`want_failed_ops` (the failed operations' indexes among those sent). `as`
labels the commit's request id and generation.

**pipeline.** Without `base`, `add`, `chain` or `remove`, reads the active
pipeline and its version (generation and daemon epoch) into `as`. Otherwise
builds on `base`'s pipeline (a label of an earlier pipeline step; the active
one if absent): `add` modules (`[name, class]`, no argument), `chain` them gate
0 to gate 0, `remove` modules (`base` alone: that pipeline again). Then `plan`
(at least one step; with `want_plan_version_of`, made against that label's
version), `want_changes` (the diff against the active pipeline has that many
changes), and the apply, expecting `at_version_of`'s version if given;
`want_generation_of`: the apply reached that label's generation (nothing
changed). `as` labels the built pipeline and the version the apply reached.

## Not yet

- Resource capability and binding information in discovery.
- A machine-readable error taxonomy beyond `ErrorDetail`'s codes: schema
  mismatch, unknown resource, unsupported capability, unknown after restart.
- Streaming transactions (the protocol has no stream yet); they will follow
  this contract.
