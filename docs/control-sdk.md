# Control SDK (`pybess.sdk`, M27)

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

## What it guarantees

| Situation | What the SDK does |
|---|---|
| One logical transaction | One `request_id` for its whole life, kept across every retry. |
| No answer (deadline, unavailable) | Asks `GetTransaction(request_id)`. Known: that outcome (`replayed`). Not known under the same daemon epoch: sends the identical request again (the daemon replays or applies it once). |
| No answer through every attempt | `TransportError`: the outcome is unknown, not failed; ask `get_transaction(request_id)` later. |
| The daemon restarted meanwhile | `DaemonRestarted`: the outcome is lost with the daemon's state; never reported as applied or as not applied. |
| `OUTCOME_BUSY` | Retries with doubling backoff, then `Busy`. |
| `OUTCOME_CONFLICT` | `Conflict` (nothing tried); the application decides what a moved generation means. |
| `OUTCOME_REJECTED` | `Rejected`, with each failing operation's index and error; nothing changed. |
| A refused call | `InvalidRequest`, with the server's `ErrorDetail` (code, field, object). |
| A key or value of the wrong type | `InvalidRequest` before anything is sent. |

Conflicts and rejections are never retried. Tuning:
`sdk.RetryPolicy(attempt_timeout, attempts, busy_backoff)` (`attempts=1`: no
retry, one status query after a timeout).

One limit: the daemon remembers outcomes in a bounded window (4096). A request
that applied and then aged out before the retry asked looks unseen and is sent
again. Where applying twice would matter, set `expected_generation`: the second
application is then a `Conflict`.
Consistency: `client.transaction(snapshot=True)` asks for
`CONSISTENCY_SCOPE_SNAPSHOT` (D-050); the default is referential.

## Tests

`pybess/test_sdk.py` scripts every recovery path against a fake stub (the
`python` suite); `bessctl/module_tests/control_sdk.py` runs a transaction,
a replay, a conflict and a client-side type refusal against a live daemon
(the `integration` suite).

## Not yet

A Go client with the same guarantees; streaming transactions (the protocol
has no stream yet). Both follow this contract when they come.
