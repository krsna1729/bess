# SPDX-License-Identifier: BSD-3-Clause
"""A thin client for BESS's generic control API (control_v2), D-146.

It makes the subtle parts of the transaction protocol hard to get wrong: one
request id per logical transaction, kept across retries; a lost answer is a
`TransportError` rather than a guess, with the request id surviving so
`get_transaction` can report what the daemon recorded; the daemon epoch, so an
outcome lost to a restart is reported as unknown rather than as "never ran";
typed resources checked before anything is sent; and typed outcomes.

It holds no application semantics: resources are whatever modules registered
("<module>/<table>"), keys and values their own protobuf messages. Schemas come
from the daemon (`GetSchema`), so the SDK ships generated code for the control
protocol only - the same choice the Go and C++ SDKs made.

    import bess
    client = bess.Client("localhost:10514")
    rules = client.resource("em0/rules")
    tx = client.transaction()
    tx.upsert(rules, key, value)
    applied = tx.commit()
"""

from .client import (
    COUNTER_THRESHOLD,
    NO_WORKER,
    Applied,
    Client,
    CounterCrossing,
    PipelineSnapshot,
    PipelineVersion,
    RetryPolicy,
    Transaction,
)
from .errors import (
    BusyError,
    ConflictError,
    DaemonRestartedError,
    Error,
    GapError,
    InvalidRequestError,
    OpFailure,
    RejectedError,
    StaleResourceError,
    TransportError,
)
from .pipeline import PipelineBuilder

__all__ = [
    "COUNTER_THRESHOLD",
    "NO_WORKER",
    "Applied",
    "BusyError",
    "Client",
    "ConflictError",
    "CounterCrossing",
    "DaemonRestartedError",
    "Error",
    "GapError",
    "InvalidRequestError",
    "OpFailure",
    "PipelineBuilder",
    "PipelineSnapshot",
    "PipelineVersion",
    "RejectedError",
    "RetryPolicy",
    "StaleResourceError",
    "Transaction",
    "TransportError",
]
