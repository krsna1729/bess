# SPDX-License-Identifier: BSD-3-Clause
"""The control_v2 client (D-146).

The client holds the parts of the protocol that are easy to get wrong when
every controller writes them again:

* one request id per logical transaction, kept across retries, so a retry is
  the same transaction and not a second one;
* a lost answer is a `TransportError`, never a guess: the request id survives
  it, and `get_transaction` reports what the daemon recorded for that id (its
  `known` flag says whether it recorded anything at all);
* the daemon epoch, so an outcome lost to a restart is reported as unknown;
* typed resources, checked before anything is sent;
* typed outcomes: every failure has its own class (`bess.errors`).
"""

import itertools
import threading
import time
import uuid
from typing import NamedTuple

import grpc

from . import errors
from .controlv2 import control_v2_pb2 as v2
from .controlv2 import control_v2_pb2_grpc as v2grpc
from .pipeline import PipelineSnapshot, PipelineVersion, Resource

# The wire's status codes that mean "ask again": the daemon was busy.
_BUSY_CODES = (v2.ErrorDetail.RESOURCE_BUSY,)
# The wire's codes that mean "your view is stale".
_CONFLICT_CODES = (v2.ErrorDetail.CONFLICT, v2.ErrorDetail.EPOCH_MISMATCH)

_ids = itertools.count(1)

# The event type every counter crossing is reported as, by both producers: a
# module's tick sweeping the totals it keeps, and a worker's own count on the
# packet path (D-150 to D-152).
COUNTER_THRESHOLD = "bess.counter_threshold"

# The `worker` of a crossing a module's tick reported: it read every worker's
# plane, so no single one crossed.
NO_WORKER = -1


class CounterCrossing(NamedTuple):
    """One counter reaching a limit a controller armed.

    Who reports it decides what `value` means. A module's tick reads every
    worker's plane, so its `value` is the key's total and `worker` is
    `NO_WORKER`. A worker reports its own plane: `worker` names it and `value`
    is that worker's share, which can be below the total the controller armed
    on -- a hint to read the total, not the answer.
    """

    source: str
    key: int
    value: int
    limit: int
    worker: int
    sequence: int
    time_ns: int


# The daemon attaches a failed call's typed ErrorDetail as this binary trailer
# (core/control/api_v2.cc:486); grpc-python's RpcError.details() is the status
# message (a str), never the detail. The Go SDK reads the same trailer
# (sdk/go/bess/client.go:60-61).
_ERROR_DETAIL_TRAILER = "bess-error-bin"


def _version(response):
    """The PipelineVersion a control answer reports."""
    return PipelineVersion(
        response.generation, response.daemon_epoch, response.resource_generation
    )


def _new_request_id():
    return f"py-{uuid.uuid4().hex[:16]}-{next(_ids)}"


class RetryPolicy:
    """One attempt's timeout, how many attempts, and the busy pause."""

    __slots__ = ("attempt_timeout", "attempts", "busy_backoff")

    def __init__(self, attempt_timeout=5.0, attempts=3, busy_backoff=0.01):
        self.attempt_timeout = attempt_timeout
        self.attempts = attempts
        self.busy_backoff = busy_backoff


class Applied:
    """A committed transaction's outcome."""

    __slots__ = ("record", "daemon_epoch", "replayed")

    def __init__(self, record, daemon_epoch, replayed):
        self.record = record
        self.daemon_epoch = daemon_epoch
        self.replayed = replayed

    @property
    def generation(self):
        return self.record.generation

    def __repr__(self):
        return (
            f"Applied(request_id={self.record.request_id!r}, "
            f"generation={self.record.generation}, replayed={self.replayed})"
        )


class Transport:
    """A gRPC channel plus the two stubs, behind one `call`."""

    def __init__(self, channel):
        self._channel = channel
        self._control = v2grpc.ControlStub(channel)
        self._admin = v2grpc.AdminStub(channel)

    def close(self):
        self._channel.close()

    def call(self, method, request, timeout=None, admin=False):
        stub = self._admin if admin else self._control
        return getattr(stub, method)(request, timeout=timeout)

    def stream(self, method, request, timeout=None, admin=False):
        stub = self._admin if admin else self._control
        return getattr(stub, method)(request, timeout=timeout)


def _detail(error):
    """The daemon's `ErrorDetail`, from the `bess-error-bin` trailer.

    It is a *trailer*, not a status detail: the daemon serializes the typed
    detail into binary trailing metadata (core/control/api_v2.cc:486) and
    leaves the status message as the human-readable text.
    """
    for key, value in error.trailing_metadata() or ():
        if key == _ERROR_DETAIL_TRAILER:
            detail = v2.ErrorDetail()
            detail.ParseFromString(value)
            return detail
    return None


def _translate(error, request_id=""):
    """Turns a gRPC failure into one of this SDK's typed errors."""
    detail = _detail(error)
    code = detail.code if detail is not None else None
    message = detail.message if detail is not None and detail.message else str(error)

    if code in _BUSY_CODES:
        return errors.BusyError(message or "the daemon is busy", detail=detail)
    if code == v2.ErrorDetail.EPOCH_MISMATCH:
        return errors.DaemonRestartedError(message or "the daemon restarted", refused=True)
    if code == v2.ErrorDetail.CONFLICT:
        return errors.ConflictError(message or "the generation moved", after_unknown_attempt=False)
    if code in (v2.ErrorDetail.INVALID_ARGUMENT, v2.ErrorDetail.NOT_FOUND,
                v2.ErrorDetail.ALREADY_EXISTS, v2.ErrorDetail.UNSUPPORTED_TRANSACTION,
                v2.ErrorDetail.RESOURCE_FAILURE):
        return errors.InvalidRequestError(message, code=code, detail=detail)
    return errors.TransportError(message, request_id=request_id, code=code)


def _applied(response):
    """`Applied` for a response whose record says APPLIED, else a typed error.

    The record's outcome is the authority, not the operations' statuses: a
    CONFLICT or BUSY record reports no applied operation at all, and a
    transaction that was never tried must not read as a success.
    """
    record = response.record
    if record.outcome == v2.TransactionRecord.OUTCOME_REJECTED:
        # Only STATUS_FAILED caused the rejection ("this operation caused the
        # rejection"); a NOT_APPLIED operation was skipped because of another
        # one and stays in record.ops for a caller to read.
        failures = [
            errors.OpFailure(index=i, error=r.error)
            for i, r in enumerate(record.ops)
            if r.status == v2.TransactionOpResult.STATUS_FAILED
        ]
        raise errors.RejectedError(
            f"{len(failures)} of {len(record.ops)} operations were not applied",
            record=record,
            failures=failures,
        )
    if record.outcome == v2.TransactionRecord.OUTCOME_CONFLICT:
        raise errors.ConflictError("the dataplane generation moved; nothing was tried")
    if record.outcome == v2.TransactionRecord.OUTCOME_BUSY:
        raise errors.BusyError("readers did not quiesce; nothing was tried")
    if record.outcome != v2.TransactionRecord.OUTCOME_APPLIED:
        raise errors.Error(
            f"the daemon sent an unspecified transaction outcome ({record.outcome})"
        )
    return Applied(record, response.daemon_epoch, response.replayed)


class Transaction:
    """One logical transaction: a request id and a list of operations.

    The request id is made once and kept across retries, so committing twice
    after a lost answer replays the same transaction instead of running a
    second one.
    """

    def __init__(self, client, snapshot=True, request_id=""):
        self._client = client
        self._ops = []
        self.request_id = request_id or _new_request_id()
        self._version = None
        if snapshot:
            snap = client.get_pipeline()
            self._version = snap.version
        # The epoch is fixed when the transaction is built, not when it is sent:
        # daemon_epoch is "the epoch the request was built under", so a
        # re-commit after a restart must be refused (EPOCH_MISMATCH) rather than
        # accepted under whatever epoch the daemon now reports.
        self._epoch = client._epoch or client.get_pipeline().version.daemon_epoch

    def _checked(self, resource, key, value=None):
        """The resource's name, after checking the key and value types.

        A `Resource` carries the daemon's own type names, so a mismatched
        message is refused here rather than by the daemon after a round trip. A
        resource named by string is looked up in the client's listing (one call
        the first time) and an unknown name is refused the same way.
        """
        if isinstance(resource, str):
            found = self._client.resources().get(resource)
            if found is None:
                # A listing read before a module was added would miss it, so one
                # miss re-reads before the name is refused.
                found = self._client.resources(refresh=True).get(resource)
            if found is None:
                raise errors.InvalidRequestError(f"no resource named {resource!r}")
            name, key_type, value_type = found.name, found.key_type, found.value_type
        else:
            name, key_type, value_type = resource.name, resource.key_type, resource.value_type
        if key_type and key.DESCRIPTOR.full_name != key_type:
            raise errors.InvalidRequestError(
                f"{name} takes a {key_type} key, not {key.DESCRIPTOR.full_name}")
        if value is not None and value_type and value.DESCRIPTOR.full_name != value_type:
            raise errors.InvalidRequestError(
                f"{name} takes a {value_type} value, not {value.DESCRIPTOR.full_name}")
        return name

    def upsert(self, resource, key, value):
        """Adds or replaces one entry. `key` and `value` are protobuf messages."""
        op = v2.TransactionOp()
        op.resource = self._checked(resource, key, value)
        op.erase = False
        op.key.Pack(key)
        op.value.Pack(value)
        self._ops.append(op)
        return self

    def erase(self, resource, key):
        """Removes one entry."""
        op = v2.TransactionOp()
        op.resource = self._checked(resource, key)
        op.erase = True
        op.key.Pack(key)
        self._ops.append(op)
        return self

    def commit(self, consistency=None):
        """Sends the transaction and returns `Applied`, or raises typed errors."""
        if not self._ops:
            raise errors.InvalidRequestError("a transaction needs at least one operation")
        request = v2.ApplyTransactionRequest()
        request.request_id = self.request_id
        request.ops.extend(self._ops)
        request.daemon_epoch = self._epoch
        if self._version is not None:
            # The dataplane transaction generation, which is NOT the pipeline's:
            # a pipeline change moves `generation`, a resource change moves
            # `resource_generation`, and the daemon refuses the request unless
            # the latter is still what this transaction was built against.
            request.expected_generation = self._version.resource_generation
        if consistency is not None:
            request.consistency = consistency

        # An OUTCOME_BUSY record means readers were slow to quiesce and nothing
        # was tried; the proto asks the client to back off and retry, and the
        # same request id keeps that a retry rather than a second transaction.
        # _call's own busy retry is off so this loop is the one budget.
        attempts = 0
        backoff = self._client.retry.busy_backoff
        while True:
            attempts += 1
            try:
                response = self._client._call(
                    "ApplyTransaction", request, request_id=self.request_id, retry_busy=False
                )
                return _applied(response)
            except errors.BusyError as busy:
                if attempts >= self._client.retry.attempts:
                    busy.attempts = attempts
                    raise
                self._client._sleep(backoff)
                backoff *= 2

    def __len__(self):
        return len(self._ops)


# The requests the daemon requires a daemon_epoch on: zero is INVALID_ARGUMENT
# (control_v2.proto, D-106). WatchEvents is deliberately absent - its epoch
# means "0: any", so sending one would narrow the stream - and so are the
# answers-only requests.
_EPOCH_REQUIRED = frozenset({
    "ApplyPipeline",
    "ApplyTransaction",
    "ImportPlugin",
    "InvokeAction",
    "PauseWorkers",
    "ResumeWorkers",
    "SetCapture",
    "Shutdown",
    "UnloadPlugin",
})


class Client:
    """A control_v2 client.

        client = Client("localhost:10514")
        snapshot = client.get_pipeline()
        tx = client.transaction()
        tx.upsert("em0/rules", key, value)
        applied = tx.commit()
    """

    def __init__(self, addr=None, channel=None, transport=None, retry=None, sleep=time.sleep):
        if transport is not None:
            self._transport = transport
        elif channel is not None:
            self._transport = Transport(channel)
        elif addr is not None:
            self._transport = Transport(grpc.insecure_channel(addr))
        else:
            raise ValueError("one of addr, channel or transport is required")
        self.retry = retry or RetryPolicy()
        self._sleep = sleep
        self._resources = None
        self._epoch = 0
        self._lock = threading.Lock()

    @classmethod
    def dial(cls, addr, **kwargs):
        return cls(addr=addr, **kwargs)

    def close(self):
        self._transport.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    # -- the one call path: translate failures, retry only busy ------------

    def _call(self, method, request, admin=False, request_id="", retry_busy=True):
        # A request the daemon requires an epoch on must send one, or it is
        # refused with INVALID_ARGUMENT (control_v2.proto, D-106): send the
        # last one any answer reported, reading one first if none has yet.
        if (method in _EPOCH_REQUIRED
                and "daemon_epoch" in request.DESCRIPTOR.fields_by_name
                and not request.daemon_epoch):
            if not self._epoch:
                self.get_pipeline()
            request.daemon_epoch = self._epoch
        attempts = 0
        backoff = self.retry.busy_backoff
        while True:
            attempts += 1
            try:
                response = self._transport.call(
                    method, request, timeout=self.retry.attempt_timeout, admin=admin
                )
            except errors.Error:
                raise
            except grpc.RpcError as rpc_error:
                translated = _translate(rpc_error, request_id=request_id)
                if isinstance(translated, errors.BusyError) and retry_busy and attempts < self.retry.attempts:
                    self._sleep(backoff)
                    backoff *= 2
                    continue
                if isinstance(translated, errors.BusyError):
                    translated.attempts = attempts
                raise translated from rpc_error
            # Requests that need an epoch (Shutdown, ApplyPipeline) must send
            # the one their resources were built under, so remember the last
            # one any answer reported (control_v2.proto: "from any earlier
            # answer").
            epoch = getattr(response, "daemon_epoch", 0)
            if epoch:
                self._epoch = epoch
            return response

    # -- desired state -----------------------------------------------------

    def get_pipeline(self, include_resources=False, resources=None):
        """The daemon's pipeline and version.

        With `include_resources`, every resource's live entries come back too;
        `resources` names which ones (none means all of them).
        """
        request = v2.GetPipelineRequest()
        request.include_resources = include_resources
        if resources:
            request.resources.extend(resources)
        response = self._call("GetPipeline", request)
        return PipelineSnapshot(response.pipeline, _version(response))

    def validate_pipeline(self, pipeline):
        """The daemon's canonical form of `pipeline`, and what each of its
        modules would promise about its state, by module name (D-180)."""
        request = v2.ValidatePipelineRequest()
        request.pipeline.CopyFrom(pipeline)
        response = self._call("ValidatePipeline", request)
        return response.normalized, dict(response.capabilities)

    def diff_pipeline(self, pipeline):
        request = v2.DiffPipelineRequest()
        request.pipeline.CopyFrom(pipeline)
        response = self._call("DiffPipeline", request)
        return response.diff, _version(response)

    def plan_pipeline(self, pipeline):
        request = v2.PlanPipelineRequest()
        request.pipeline.CopyFrom(pipeline)
        response = self._call("PlanPipeline", request)
        return list(response.steps), _version(response)

    def apply_pipeline(self, pipeline, expected=None):
        """Applies a pipeline. `expected` is a PipelineVersion to require."""
        request = v2.ApplyPipelineRequest()
        request.pipeline.CopyFrom(pipeline)
        if expected is not None:
            request.expected_generation = expected.generation
            request.daemon_epoch = expected.daemon_epoch
        else:
            snap = self.get_pipeline()
            request.daemon_epoch = snap.version.daemon_epoch
        response = self._call("ApplyPipeline", request)
        # A module the apply added or removed changes the resource listing.
        self._resources = None
        return response

    # -- transactions ------------------------------------------------------

    def transaction(self, snapshot=True, request_id=""):
        return Transaction(self, snapshot=snapshot, request_id=request_id)

    def get_transaction(self, request_id):
        """What the daemon recorded for `request_id`: `(known, record, epoch)`.

        `known` is False when it has no outcome for that id - outcomes live in
        a bounded window while the daemon runs, so a restarted daemon knows
        none of them - and then `record` is unset. Returning the record alone
        would let an empty record read as "nothing was applied".
        """
        request = v2.GetTransactionRequest()
        request.request_id = request_id
        response = self._call("GetTransaction", request, request_id=request_id)
        return response.known, response.record, response.daemon_epoch

    # -- discovery ---------------------------------------------------------

    def resources(self, refresh=False):
        with self._lock:
            if self._resources is None or refresh:
                response = self._call("ListTransactionResources", v2.ListTransactionResourcesRequest())
                self._resources = {
                    r.name: Resource(
                        r.name, r.key_type, r.value_type, r.kind, r.requires_paused_workers
                    )
                    for r in response.resources
                }
            return dict(self._resources)

    def resource(self, name):
        found = self.resources().get(name)
        if found is None:
            # One miss re-reads: a listing read before a module was added would
            # miss it, and a name that does exist must not read as a client-side
            # error.
            found = self.resources(refresh=True).get(name)
        if found is None:
            raise errors.InvalidRequestError(f"no resource named {name!r}")
        return found

    def capabilities(self):
        return self._call("GetCapabilities", v2.GetCapabilitiesRequest())

    def supports(self, rpc):
        """True if the daemon serves `rpc`, given by its full name
        ("bess.pb.v2.Control/ApplyTransaction")."""
        return rpc in self.capabilities().rpcs

    def schema(self):
        return self._call("GetSchema", v2.GetSchemaRequest())

    def metrics(self):
        return list(self._call("ListMetrics", v2.ListMetricsRequest()).samples)

    def invoke_action(self, module, action, arg=None):
        request = v2.InvokeActionRequest()
        request.module = module
        request.action = action
        if arg is not None:
            request.arg.Pack(arg)
        return self._call("InvokeAction", request).result

    # -- administration (the Admin service, D-107) -------------------------

    def import_plugin(self, path):
        request = v2.ImportPluginRequest()
        request.path = path
        return self._call("ImportPlugin", request, admin=True)

    def unload_plugin(self, path):
        request = v2.UnloadPluginRequest()
        request.path = path
        return self._call("UnloadPlugin", request, admin=True)

    def port_status(self):
        return self._call("GetPortStatus", v2.GetPortStatusRequest(), admin=True)

    def packet_pools(self):
        return self._call("GetPacketPools", v2.GetPacketPoolsRequest(), admin=True)

    def set_capture(self, **kwargs):
        """Starts or stops a capture. See `SetCaptureRequest` for the fields."""
        request = v2.SetCaptureRequest(**kwargs)
        return self._call("SetCapture", request, admin=True)

    def pause_workers(self):
        return self._call("PauseWorkers", v2.PauseWorkersRequest(), admin=True)

    def resume_workers(self):
        return self._call("ResumeWorkers", v2.ResumeWorkersRequest(), admin=True)

    def shutdown(self):
        """Asks the daemon to stop: it stops its workers and exits.

        The epoch comes from the last answer (the request needs one), which
        `_call` fills in.
        """
        return self._call("Shutdown", v2.ShutdownRequest(), admin=True)

    def watch_events(self, types=None, timeout=None, from_sequence=0, daemon_epoch=0):
        """Yields events until the stream ends or `timeout` expires.

        A deadline the caller set is how a watch ends, so it is not an error:
        the stream stops and the generator returns. Any other transport failure
        is a typed error, like every other call's.

        `from_sequence` resumes where an earlier stream stopped and
        `daemon_epoch` says which epoch that sequence belongs to (0: any), so a
        caller can continue after a dropped stream instead of missing events or
        seeing them twice. The daemon starts such a stream with a "bess.restart"
        event if it restarted since, which is how the gap becomes visible.
        """
        request = v2.WatchEventsRequest()
        if types:
            request.types.extend(types)
        request.from_sequence = from_sequence
        request.daemon_epoch = daemon_epoch
        try:
            for event in self._transport.stream("WatchEvents", request, timeout=timeout):
                yield event
        except grpc.RpcError as rpc_error:
            if rpc_error.code() == grpc.StatusCode.DEADLINE_EXCEEDED:
                return
            raise _translate(rpc_error) from rpc_error

    def watch_counters(self, module, timeout=None, from_sequence=0, daemon_epoch=0):
        """Yields the counter crossings `module` reports, as `CounterCrossing`s.

        `watch_events` narrowed to `COUNTER_THRESHOLD` and to one source, with
        the event's text fields parsed into numbers. Arming is the module's own
        business: a module that watches counters keeps its quota set as one of
        its resources, so a controller arms it by setting that resource in a
        transaction -- `tx.set(client.resource("<name>/quotas"), quotas)` for a
        module that keeps them as a singleton -- and reads it back the same way.
        Discovery names it: `client.resources()` lists what the module has.
        `timeout` is how a watch ends, exactly as for `watch_events`.

        Three things end the watch besides the deadline, each by raising,
        because a controller that misses a crossing has missed a quota:

        * a crossing whose fields do not parse (`ValueError`): the event type
          is a name any producer may register, so a silent skip could hide the
          watched module's own shape changing;
        * a gap in the log (`errors.GapError`): events the daemon no longer
          holds, which may include one of this module's crossings;
        * a restart (`errors.DaemonRestartedError`): everything built from
          earlier events is void.

        After either of the last two, re-read the counters and watch again. The
        daemon's other control events never reach this helper: the request's
        type filter drops them. A worker's ring refusing an event is reported
        as "bess.events_lost", which names the worker rather than a module, so
        a worker-ring loss can drop this module's own crossings without this
        watch saying so -- a controller that must not miss one watches
        `watch_events` directly instead. The watch starts live unless
        `from_sequence` says otherwise.
        """
        for event in self.watch_events(
            types=[COUNTER_THRESHOLD],
            timeout=timeout,
            from_sequence=from_sequence,
            daemon_epoch=daemon_epoch,
        ):
            if event.type == "bess.gap":
                raise errors.GapError(
                    f"bess: the event log dropped events {event.gap_from} to "
                    f"{event.gap_to}, so a crossing of {module!r} may be "
                    f"missing: re-read its counters",
                    module=module,
                    gap_from=event.gap_from,
                    gap_to=event.gap_to,
                )
            if event.type == "bess.restart":
                raise errors.DaemonRestartedError(
                    f"bess: the daemon restarted (epoch {event.daemon_epoch}), "
                    f"so a watch of {module!r} must start again"
                )
            if event.type != COUNTER_THRESHOLD or event.source != module:
                continue
            yield _crossing(event)


def _crossing(event):
    """The `CounterCrossing` an event carries, or `ValueError` if it cannot."""

    def field(name, limit):
        text = event.fields.get(name)
        if text is None:
            raise ValueError(
                f"bess: crossing from {event.source!r} has no {name!r} field "
                f"(fields: {dict(event.fields)})"
            )
        # Go's strconv.ParseUint rule and range, so both SDKs accept and reject
        # the same events: ASCII digits only, no sign, no whitespace, no
        # underscores, and inside the field's width. int() alone would take
        # "1_000", " 7 " and "-5" into a uint64, and would take a magnitude Go
        # refuses.
        if not text.isascii() or not text.isdigit():
            raise ValueError(
                f"bess: crossing from {event.source!r} has {name} {text!r}, "
                f"which is not a number"
            )
        value = int(text)
        if value > limit:
            raise ValueError(
                f"bess: crossing from {event.source!r} has {name} {text!r}, "
                f"which is above {limit}"
            )
        return value

    worker = NO_WORKER
    if "worker" in event.fields:
        # Go's CounterCrossing.Worker is an int, so the same bound applies
        # there: a value above it is refused rather than narrowed.
        worker = field("worker", 2**31 - 1)
    return CounterCrossing(
        source=event.source,
        key=field("key", 2**64 - 1),
        value=field("value", 2**64 - 1),
        limit=field("limit", 2**64 - 1),
        worker=worker,
        sequence=event.sequence,
        time_ns=event.time_ns,
    )
