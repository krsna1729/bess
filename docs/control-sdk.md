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

## What they guarantee

| Situation | What the SDK does |
|---|---|
| One logical transaction | One `request_id` for its whole life, kept across every retry. |
| Before the first send | Learns the daemon epoch (resource discovery) if this client has not. |
| No answer (deadline, unavailable; `INTERNAL`/`UNKNOWN` without BESS's error detail) | Asks `GetTransaction(request_id)`, again if the question goes unanswered too. Known: that outcome (`replayed`). Answered "not known" under the epoch the commit started with: sends the identical request again (the daemon replays or applies it once). Never sends again on a guess. |
| No answer through every attempt | `TransportError`: the outcome is unknown, not failed; ask `get_transaction(request_id)` later. |
| Any answer from another daemon epoch | `DaemonRestarted`: the transaction met a daemon that lost the state it was built against; never reported as applied or as not applied (the new daemon may have applied it: re-read its state). |
| `OUTCOME_BUSY` | Retries with doubling backoff, then `Busy`. |
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
stale. A restart the client has not seen yet passes these checks: the request
is sent, and the restarted daemon may apply it (it decodes keys and values by
type, so a resource whose schema changed under the same name refuses it). The
answer's new epoch then raises `DaemonRestarted`, which means "the outcome on
the new daemon is not reported": re-read its state before deciding, as a retry
under a new request id could apply the transaction twice.

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

The Go client follows the same contract (`sdk/go/bess/client.go`); its scripted
tests cover the same cases.


`pybess/test_sdk.py` and `sdk/go/bess/client_test.go` script every recovery
path against a fake transport (the `python` suite; the CI job "Go SDK", with
the race detector). Against a live daemon (the `integration` suite):
`bessctl/module_tests/control_sdk.py` (Python) and `control_sdk_go.py`, which
runs the Go client (`go test -tags live`) to commit a rule, replay it, meet a
conflict and refuse a mistyped key, then checks the rule steers packets.

## Not yet

This is M27's first slice (transactional control, Python and Go). Still to
come, as the roadmap's M27 describes:

- Resource capability and binding information in discovery.
- A machine-readable error taxonomy beyond `ErrorDetail`'s codes: schema
  mismatch, unknown resource, unsupported capability, unknown after restart.
- A request field naming the epoch the transaction was built for, so the
  daemon itself refuses one sent across a restart the client has not seen
  (today the client reports it as `DaemonRestarted` after the fact).
- Desired-state, capability, telemetry and event surfaces (M25's metrics and
  events feed the last two).
- Streaming transactions (the protocol has no stream yet); they will follow
  this contract.
