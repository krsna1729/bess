# Control SDKs (`pybess.sdk`, Go `sdk/go`, M27)

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
`*DaemonRestartedError`, `*InvalidRequestError`):

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

## What they guarantee

| Situation | What the SDK does |
|---|---|
| One logical transaction | One `request_id` for its whole life, kept across every retry. |
| Before the first send | Learns the daemon epoch (resource discovery) if this client has not. |
| No answer (deadline, unavailable; `INTERNAL`/`UNKNOWN` without BESS's error detail) | Asks `GetTransaction(request_id)`, again if the question goes unanswered too. Known: that outcome (`replayed`). Answered "not known" under the epoch the commit started with: sends the identical request again (the daemon replays or applies it once). Never sends again on a guess. |
| No answer through every attempt | `TransportError`: the outcome is unknown, not failed; ask `get_transaction(request_id)` later. |
| Any answer from another daemon epoch | `DaemonRestarted`: the transaction met a daemon that lost the state it was built against; never reported as applied or as not applied. |
| `OUTCOME_BUSY` | Retries with doubling backoff, then `Busy`. |
| `OUTCOME_CONFLICT` | `Conflict` (nothing tried); the application decides what a moved generation means. `after_unknown_attempt`: an earlier send of the same commit went unanswered and may have applied. |
| `OUTCOME_REJECTED` | `Rejected`, with each failing operation's index and error; nothing changed. |
| A refused call | `InvalidRequest`, with the server's `ErrorDetail` (code, field, object). |
| A key or value of the wrong type | `InvalidRequest` before anything is sent. |

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

## Tests

`pybess/test_sdk.py` and `sdk/go/bess/client_test.go` script every recovery
path against a fake transport (the `python` suite; the CI job "Go SDK", with
the race detector). Against a live daemon (the `integration` suite):
`bessctl/module_tests/control_sdk.py` (Python) and `control_sdk_go.py`, which
runs the Go client (`go test -tags live`) to commit a rule, replay it, meet a
conflict and refuse a mistyped key, then checks the rule steers packets.

## Not yet

Streaming transactions (the protocol has no stream yet); it will follow this
contract when it comes.
