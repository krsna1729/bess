# BESS Python SDK (control_v2)

A thin client for BESS's generic control API (`protobuf/control_v2.proto`),
the Python member of the three first-class SDKs (D-146; the others are
`sdk/cpp` and `sdk/go`).

It adds no semantics of its own: resources are whatever modules registered
(`<module>/<table>`), keys and values their own protobuf messages. What it adds
is the part of the protocol that is easy to get wrong when every controller
writes it again:

* **one request id per logical transaction**, kept across retries, so a retry
  is the same transaction and not a second one;
* **a lost answer is a `TransportError`, not a guess**: the request id
  survives it, and `get_transaction` reports what the daemon recorded for that
  id (its `known` flag says whether it recorded anything at all);
* **the daemon epoch**, so an outcome lost to a restart is reported as unknown
  rather than as "never ran";
* **typed resources**, checked before anything is sent;
* **typed outcomes**: every failure has its own class (`bess.errors`).

Schemas come from the daemon (`GetSchema`'s `FileDescriptorSet`), so this
package ships generated code for the control protocol only.

## Install

```sh
pip install ./sdk/python
```

It needs `grpcio` and `protobuf`, and nothing else. It is not part of the
runtime package: `bessd`, `bessctl` and the C++ SDK do not depend on it, and a
machine without Python keeps a working daemon.

## Use

```python
import bess

client = bess.Client("localhost:10514")
rules = client.resource("em0/rules")          # typed: key/value message names
snapshot = client.get_pipeline()               # pipeline + version

tx = client.transaction()                      # one request id, kept across retries
tx.upsert(rules, key, value)                   # any protobuf message
applied = tx.commit()

# desired state, as one call
plan = client.plan_pipeline(snapshot.pipeline)
response = client.apply_pipeline(snapshot.pipeline, expected=snapshot.version)
```

Errors: `InvalidRequestError`, `ConflictError`, `RejectedError` (with the
failing operations), `BusyError`, `TransportError` (with the request id to
recover with), `StaleResourceError`, `DaemonRestartedError`. All derive from
`bess.Error`.

## Test

```sh
cd sdk/python && python3 -m unittest discover -s tests -t .
```

The unit tests run against a scripted transport - no daemon, no network - and
they carry the daemon's typed error the way the wire does (the
`bess-error-bin` trailer), so a client that stopped reading the trailer would
fail them.

## Regenerate the wire code

```sh
tools/gen_python_sdk.sh
```

The output under `bess/controlv2/` is committed, and CI regenerates it and
diffs, so the committed code cannot drift from the protos. The generator needs
`grpcio-tools` only (pinned to the version CI installs, since its bundled
`protoc` is what emits the messages, the stubs and the type stubs); the SDK
itself needs neither. The pin alone does not make the bytes: the same
`grpcio-tools` release stamps different protobuf versions depending on which
protobuf its packager built it against, so the generator normalizes that one
stamp to the floor `pyproject.toml` declares.
