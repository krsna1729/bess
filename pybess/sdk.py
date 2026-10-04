# SPDX-License-Identifier: BSD-3-Clause
"""A thin client for BESS's generic control API (control_v2), M27.

It makes the subtle parts of the transaction protocol hard to get wrong: one
request id per logical transaction, kept across retries; recovery after a
timeout through GetTransaction instead of guessing; the daemon epoch, so an
outcome lost to a restart is reported as unknown rather than as "never ran";
typed resources checked before anything is sent; and typed outcomes.

It holds no application semantics: resources are whatever modules registered
("<module>/<table>"), keys and values their own protobuf messages.

    client = Client('localhost:10514')
    routes = client.resource('rt0/routes')
    with client.transaction() as tx:
        tx.upsert(routes, key, value)
    result = tx.result          # Applied: generation, per-op results

Failures raise: Conflict (expected_generation moved), Rejected (an operation
failed; nothing changed), Busy (still busy after the retries), TransportError
(no answer at all: the outcome is unknown, not failed), DaemonRestarted (the
epoch changed: the outcome is lost with the daemon's state; re-read it),
InvalidRequest (refused before anything was applied, with the server's
ErrorDetail), StaleResource (a resource handle from before a restart the
client has seen; nothing was sent). Conflicts and rejections are never retried.

A Resource is bound to the daemon epoch it was discovered under. After the
client sees another epoch, an old handle is refused, never sent: look the
resource up again (client.resource(name)), as the restarted daemon may serve
another set. A transaction is bound to its first resource's epoch.

Desired state (the pipeline): PipelineBuilder assembles a control_v2.Pipeline
(or edits a snapshot from client.pipeline()); the daemon validates, diffs,
plans and applies it -- the SDK repeats none of that:

    snap = client.pipeline()
    p = sdk.PipelineBuilder(snap.pipeline).module('em', 'ExactMatch', arg).connect('rnd', 'em')
    client.apply_pipeline(p.build(), expected_generation=snap.generation)

apply_pipeline retries a busy answer (bounded, like a transaction) and never a
conflict or a refusal. It has no request id: after no answer, the outcome is
unknown (TransportError) -- read client.pipeline() and compare before applying
again, or apply with expected_generation so a second application conflicts.

One limit: the daemon remembers outcomes in a bounded window (4096). A request
that applied, then aged out of it before the retry asked, looks unseen and is
sent again. Set expected_generation where applying twice would matter: the
second application is then a Conflict.
"""

import time
import uuid

import grpc

from google.protobuf import any_pb2

try:
    from builtin_pb import control_v2_pb2 as v2
    from builtin_pb import control_v2_pb2_grpc as v2_grpc
except ImportError:  # imported as pybess.sdk with pybess's parent on the path
    from pybess.builtin_pb import control_v2_pb2 as v2
    from pybess.builtin_pb import control_v2_pb2_grpc as v2_grpc

_OUTCOME = v2.TransactionRecord
_REFERENTIAL = v2.ApplyTransactionRequest.CONSISTENCY_REFERENTIAL
_SCOPE_SNAPSHOT = v2.ApplyTransactionRequest.CONSISTENCY_SCOPE_SNAPSHOT


# -- errors -----------------------------------------------------------------------

class Error(Exception):
    """Base of every SDK error."""


class InvalidRequest(Error):
    """Refused before anything was applied (bad resource, key or value)."""

    def __init__(self, message, detail=None, code=None):
        super().__init__(message)
        self.detail = detail  # control_v2.ErrorDetail, when the server sent one
        self.code = code      # the call's grpc.StatusCode (None: refused by the client)


class Conflict(Error):
    """expected_generation no longer matched: this attempt tried nothing.

    after_unknown_attempt: an earlier send of the same commit went unanswered,
    so that attempt may have applied (and moved the generation itself) with
    its record since aged out of the daemon's window: re-read the state."""

    def __init__(self, record, after_unknown_attempt=False):
        super().__init__('generation moved on (now %d)%s' % (
            record.generation, '; an earlier unanswered attempt may have applied'
            if after_unknown_attempt else ''))
        self.record = record
        self.after_unknown_attempt = after_unknown_attempt


class Rejected(Error):
    """An operation failed and nothing changed. `failures`: (index, error)."""

    def __init__(self, record):
        self.record = record
        self.failures = [(i, op.error) for i, op in enumerate(record.ops)
                         if op.status == v2.TransactionOpResult.STATUS_FAILED]
        super().__init__('transaction rejected: %s' % '; '.join(
            '#%d %s' % f for f in self.failures))


class Busy(Error):
    """The dataplane stayed busy (readers slow to quiesce) through retries."""


class TransportError(Error):
    """No answer through every attempt: the outcome is unknown. It is not a
    failure of the transaction; ask get_transaction(request_id) later."""

    def __init__(self, request_id, cause):
        super().__init__('no answer for %s: %s' % (request_id, cause))
        self.request_id = request_id
        self.cause = cause


class StaleResource(Error):
    """A resource handle (or a transaction built with it) from an earlier
    daemon epoch than the one the client has seen. Nothing was sent: look the
    resource up again and rebuild the transaction."""


class PipelineConflict(Conflict):
    """apply_pipeline's expected_generation no longer matched: nothing changed."""

    def __init__(self, message, detail=None):
        Error.__init__(self, message)
        self.record = None
        self.after_unknown_attempt = False
        self.detail = detail


class DaemonRestarted(Error):
    """The daemon restarted: the state the transaction was built against is
    gone. The restarted daemon may have applied the request (a restart the
    client had not seen yet): re-read its state before deciding again; a
    retry under a new request id could apply it twice."""


# -- results ---------------------------------------------------------------------

class Applied:
    """A transaction that was applied."""

    def __init__(self, record, replayed, epoch):
        self.record = record
        self.request_id = record.request_id
        self.generation = record.generation
        self.visibility = record.visibility
        self.replayed = replayed  # an earlier identical request's recorded outcome
        self.daemon_epoch = epoch

    def __repr__(self):
        return 'Applied(generation=%d, ops=%d%s)' % (
            self.generation, len(self.record.ops), ', replayed' if self.replayed else '')


class Resource:
    """A transactional resource, its key and value message types, and the
    daemon epoch it was discovered under (valid only in that epoch)."""

    def __init__(self, name, key_type, value_type, daemon_epoch):
        self.name = name
        self.key_type = key_type
        self.value_type = value_type
        self.daemon_epoch = daemon_epoch

    def __repr__(self):
        return 'Resource(%r, key=%s, value=%s, epoch=%d)' % (
            self.name, self.key_type, self.value_type, self.daemon_epoch)


class RetryPolicy:
    """How long one attempt may take, and how often to try again."""

    def __init__(self, attempt_timeout=5.0, attempts=4, busy_backoff=0.01):
        self.attempt_timeout = attempt_timeout  # seconds per RPC
        self.attempts = attempts                # in total, the first included
        self.busy_backoff = busy_backoff        # seconds, doubled per retry


# -- the client ------------------------------------------------------------------

_TRANSIENT = (grpc.StatusCode.DEADLINE_EXCEEDED, grpc.StatusCode.UNAVAILABLE,
              grpc.StatusCode.CANCELLED)


class Client:
    """One daemon's control endpoint. `stub` replaces the gRPC stub (tests)."""

    MAX_MESSAGE_BYTES = 64 * 1024 * 1024  # what bessd accepts (D-026)

    def __init__(self, address='localhost:10514', retry=None, stub=None, sleep=time.sleep):
        if stub is None:
            channel = grpc.insecure_channel(address, options=[
                ('grpc.max_receive_message_length', self.MAX_MESSAGE_BYTES),
                ('grpc.max_send_message_length', self.MAX_MESSAGE_BYTES)])
            stub = v2_grpc.ControlStub(channel)
        self._stub = stub
        self._retry = retry or RetryPolicy()
        self._sleep = sleep
        self._resources = None
        self.daemon_epoch = None  # the last epoch the daemon reported

    # resources

    def resources(self, refresh=False):
        """Every transactional resource, by name (cached)."""
        if self._resources is None or refresh:
            try:
                response = self._call(self._stub.ListTransactionResources,
                                      v2.ListTransactionResourcesRequest())
            except grpc.RpcError as e:
                if _no_answer(e):
                    raise TransportError(None, e.code())
                raise _translate(e)
            self._observe_epoch(response.daemon_epoch)
            self._resources = {r.name: Resource(r.name, r.key_type, r.value_type,
                                                response.daemon_epoch)
                               for r in response.resources}
        return self._resources

    def resource(self, name):
        """The resource `name`; InvalidRequest if no module registered it."""
        found = self.resources().get(name)
        if found is None:
            found = self.resources(refresh=True).get(name)
        if found is None:
            raise InvalidRequest('no transactional resource %r' % name)
        return found

    # discovery

    def capabilities(self):
        """What the daemon offers (control_v2.GetCapabilities): version, RPCs,
        plugin API and granted capabilities, module classes, plugins, ports and
        resources. Raw message; supports() answers the common question."""
        response = self._read(self._stub.GetCapabilities, v2.GetCapabilitiesRequest())
        self._observe_epoch(response.daemon_epoch)
        return response

    def supports(self, rpc):
        """Whether the daemon serves `rpc` ('ApplyTransaction', or a full name).
        A daemon too old to answer GetCapabilities (UNIMPLEMENTED) serves none
        of the newer RPCs; any other refusal is raised."""
        try:
            rpcs = self.capabilities().rpcs
        except InvalidRequest as e:
            if e.code == grpc.StatusCode.UNIMPLEMENTED:
                return False
            raise
        return any(r == rpc or r.endswith('/' + rpc) for r in rpcs)

    def watch_events(self, from_sequence=0, types=(), reconnect=True, daemon_epoch=0):
        """Operational events (control_v2.WatchEvents), oldest first, until
        the caller stops iterating.

        Starts at `from_sequence` (0: the next event); to resume from a saved
        sequence, pass the epoch it belongs to. A "bess.gap" event names events
        the daemon no longer held. After a transport failure the stream resumes
        where it stopped (`reconnect`), even before its first event. When the
        daemon restarted, DaemonRestarted is raised: what the caller built from
        earlier events is gone; re-read the state and watch from 0. The
        stream-control events "bess.start" and "bess.progress" are not yielded.
        """
        next_sequence, epoch = from_sequence, daemon_epoch or None
        backoff = self._retry.busy_backoff
        while True:
            request = v2.WatchEventsRequest(from_sequence=next_sequence, types=list(types),
                                            daemon_epoch=epoch or 0)
            try:
                for event in self._stub.WatchEvents(request):
                    if event.type == 'bess.restart' or (epoch is not None and
                                                         event.daemon_epoch != epoch):
                        self._observe_epoch(event.daemon_epoch)
                        raise DaemonRestarted('events of daemon epoch %s ended: the daemon is '
                                              'now at epoch %d' % (epoch, event.daemon_epoch))
                    epoch = event.daemon_epoch
                    if event.type == 'bess.start':
                        next_sequence = event.sequence
                        continue
                    if event.type == 'bess.progress':
                        next_sequence = event.sequence + 1
                        continue
                    next_sequence = event.gap_to if event.type == 'bess.gap' else event.sequence + 1
                    yield event
                return
            except grpc.RpcError as e:
                if not _no_answer(e):
                    raise _translate(e)
                if not reconnect:
                    raise TransportError(None, e.code())
                self._sleep(backoff)
                backoff = min(backoff * 2, 1.0)

    def metrics(self):
        """Every metric sample (control_v2.ListMetrics), as
        {(name, ((label, value), ...)): value}."""
        response = self._read(self._stub.ListMetrics, v2.ListMetricsRequest())
        self._observe_epoch(response.daemon_epoch)
        return {(m.name, tuple(sorted(m.labels.items()))): m.value for m in response.samples}

    # desired state

    def pipeline(self):
        """The active pipeline as desired state, and its generation."""
        response = self._read(self._stub.GetPipeline, v2.GetPipelineRequest())
        return PipelineSnapshot(response.pipeline, response.generation)

    def validate_pipeline(self, pipeline):
        """The daemon's canonical form of `pipeline` (InvalidRequest if invalid)."""
        return self._pipeline_call(self._stub.ValidatePipeline,
                                   v2.ValidatePipelineRequest(pipeline=pipeline)).normalized

    def diff_pipeline(self, pipeline):
        """(PipelineDiff, generation): what applying `pipeline` would change."""
        response = self._pipeline_call(self._stub.DiffPipeline,
                                       v2.DiffPipelineRequest(pipeline=pipeline))
        return response.diff, response.generation

    def plan_pipeline(self, pipeline):
        """([PlanStep], generation): the daemon's plan for `pipeline`."""
        response = self._pipeline_call(self._stub.PlanPipeline,
                                       v2.PlanPipelineRequest(pipeline=pipeline))
        return list(response.steps), response.generation

    def apply_pipeline(self, pipeline, expected_generation=None):
        """Makes `pipeline` the active one, all or nothing; returns the
        daemon's ApplyPipelineResponse (generation, applied_ops, timings)."""
        request = v2.ApplyPipelineRequest(pipeline=pipeline)
        if expected_generation is not None:
            request.expected_generation = expected_generation
        return self._pipeline_call(self._stub.ApplyPipeline, request, retry_busy=True)

    # transactions

    def transaction(self, expected_generation=None, snapshot=False, request_id=None):
        """A transaction to fill and commit; also a context manager that
        commits on a clean exit."""
        return Transaction(self, expected_generation, snapshot, request_id)

    def get_transaction(self, request_id):
        """(known, record) for `request_id` under the current epoch."""
        response = self._call(self._stub.GetTransaction,
                              v2.GetTransactionRequest(request_id=request_id))
        return response.known, response.record, response.daemon_epoch

    # internals

    def _call(self, method, request):
        return method(request, timeout=self._retry.attempt_timeout)

    def _read(self, method, request):
        """A read-only RPC: transport failures and server refusals typed.
        UNIMPLEMENTED (an older daemon) is InvalidRequest."""
        try:
            return self._call(method, request)
        except grpc.RpcError as e:
            if _no_answer(e):
                raise TransportError(None, e.code())
            raise _translate(e)

    def _pipeline_call(self, method, request, retry_busy=False):
        """A desired-state RPC. Answered refusals carry an ErrorDetail: a
        generation conflict is PipelineConflict, a busy resource is retried
        (retry_busy) up to the attempt budget; no answer is TransportError."""
        backoff = self._retry.busy_backoff
        for attempt in range(self._retry.attempts):
            try:
                return self._call(method, request)
            except grpc.RpcError as e:
                detail = _detail(e)
                if detail is None and _no_answer(e):
                    raise TransportError(None, e.code())
                if detail is not None and detail.code == v2.ErrorDetail.CONFLICT:
                    raise PipelineConflict(detail.message or e.details(), detail)
                if (detail is not None and detail.code == v2.ErrorDetail.RESOURCE_BUSY and
                        retry_busy and attempt + 1 < self._retry.attempts):
                    self._sleep(backoff)
                    backoff *= 2
                    continue
                if detail is not None and detail.code == v2.ErrorDetail.RESOURCE_BUSY:
                    raise Busy(detail.message or e.details())
                raise _translate(e)
        raise Busy('still busy after %d attempts' % self._retry.attempts)

    def _observe_epoch(self, epoch):
        changed = self.daemon_epoch is not None and epoch != self.daemon_epoch
        self.daemon_epoch = epoch
        if changed:
            self._resources = None  # the restarted daemon's modules may differ
        return changed

    def _check_bound(self, epoch, what):
        if self.daemon_epoch is None:
            self.resources(refresh=True)  # learn the epoch before anything is sent
        if epoch != self.daemon_epoch:
            raise StaleResource('%s belongs to daemon epoch %d; the daemon is now at %d: '
                                'look the resource up again' % (what, epoch, self.daemon_epoch))

    def _commit(self, request, epoch):
        """Applies `request` exactly once, whatever the transport does.

        Every RPC counts against the attempt budget. After a send without an
        answer the outcome is asked for, never assumed; the request is sent
        again only once GetTransaction has answered "not known" under the
        epoch this commit started with. An answer from any other epoch is
        DaemonRestarted: the transaction met a daemon that lost the state
        the caller built it against.
        """
        backoff = self._retry.busy_backoff
        transport_failure = None  # the last RPC that went unanswered
        last_unanswered = None    # its code, kept after later answers
        unknown_attempt = False   # a send of this commit went unanswered
        send = True
        for _ in range(self._retry.attempts):
            if send:
                try:
                    response = self._call(self._stub.ApplyTransaction, request)
                except grpc.RpcError as e:
                    if not _no_answer(e):
                        raise _translate(e)
                    transport_failure = last_unanswered = e.code()
                    unknown_attempt = True
                    send = False  # ask before sending again
                    continue
                self._check_epoch(response.daemon_epoch, epoch)
                record = response.record
                if record.outcome == _OUTCOME.OUTCOME_BUSY:
                    transport_failure = None
                    self._sleep(backoff)
                    backoff *= 2
                    continue
                return _finish(record, response.replayed, response.daemon_epoch,
                               unknown_attempt)
            try:
                known, record, answered_epoch = self.get_transaction(request.request_id)
            except grpc.RpcError as e:
                if not _no_answer(e):
                    # The question was refused, but the unanswered send may
                    # have applied: the outcome is unknown, not a refusal.
                    raise TransportError(request.request_id, e.code())
                transport_failure = last_unanswered = e.code()
                continue  # still no answer: ask again, never resend blind
            self._check_epoch(answered_epoch, epoch)
            if known:
                return _finish(record, True, answered_epoch, unknown_attempt)
            transport_failure = None
            send = True  # confirmed unseen under this epoch: sending again is safe
        # Out of attempts. If any send went unanswered, the outcome is unknown:
        # "not known" only means "not applied yet" (an unanswered Apply can
        # still be waiting for the daemon's lock), so Busy -- nothing applied --
        # would invite a retry under a new id and a second application.
        if transport_failure is not None or unknown_attempt:
            raise TransportError(request.request_id, transport_failure or last_unanswered)
        raise Busy('still busy after %d attempts' % self._retry.attempts)

    def _check_epoch(self, answered, expected):
        self._observe_epoch(answered)
        if answered != expected:
            raise DaemonRestarted('the daemon restarted (epoch %d -> %d): the transaction met a '
                                  'daemon that lost the state it was built against'
                                  % (expected, answered))


def _no_answer(error):
    """Whether a failed call says nothing about the outcome: the transport's
    timeouts and losses, and INTERNAL/UNKNOWN without BESS's error detail (a
    reset stream, a handler that died after recording)."""
    code = error.code()
    if code in _TRANSIENT:
        return True
    if code in (grpc.StatusCode.INTERNAL, grpc.StatusCode.UNKNOWN):
        return not any(key == 'bess-error-bin' for key, _ in (error.trailing_metadata() or ()))
    return False


def _finish(record, replayed, epoch, unknown_attempt=False):
    if record.outcome == _OUTCOME.OUTCOME_APPLIED:
        return Applied(record, replayed, epoch)
    if record.outcome == _OUTCOME.OUTCOME_CONFLICT:
        raise Conflict(record, unknown_attempt)
    if record.outcome == _OUTCOME.OUTCOME_REJECTED:
        raise Rejected(record)
    if record.outcome == _OUTCOME.OUTCOME_BUSY:
        raise Busy('the dataplane was busy')
    raise Error('unexpected outcome %d' % record.outcome)


def _detail(error):
    """The server's ErrorDetail of a failed call, or None."""
    for key, value in (error.trailing_metadata() or ()):
        if key == 'bess-error-bin':
            detail = v2.ErrorDetail()
            detail.ParseFromString(value)
            return detail
    return None


def _translate(error):
    """A failed call's gRPC error as an SDK error (ErrorDetail decoded)."""
    detail = _detail(error)
    message = detail.message if detail is not None and detail.message else error.details()
    if detail is not None and detail.code == v2.ErrorDetail.CONFLICT:
        return InvalidRequest('request id reused with different contents: ' + message, detail,
                              error.code())
    return InvalidRequest(message, detail, error.code())


class Transaction:
    """Operations on any resources, applied all or nothing by commit()."""

    def __init__(self, client, expected_generation, snapshot, request_id):
        self._client = client
        self._expected = expected_generation
        self._snapshot = snapshot
        # One id for the whole life of the transaction: a retry after a
        # timeout must be recognised as the same request.
        self.request_id = request_id or uuid.uuid4().hex
        self._ops = []
        self._epoch = None  # the epoch of the resources the ops were built with
        self.result = None

    def upsert(self, resource, key, value):
        """Add `key` -> `value` to `resource`, or replace its value."""
        resource = self._resolve(resource)
        _check_type(resource, 'key', key, resource.key_type)
        _check_type(resource, 'value', value, resource.value_type)
        self._ops.append(v2.TransactionOp(resource=resource.name, key=_pack(key),
                                          value=_pack(value)))
        return self

    def erase(self, resource, key):
        """Remove `key` from `resource`."""
        resource = self._resolve(resource)
        _check_type(resource, 'key', key, resource.key_type)
        self._ops.append(v2.TransactionOp(resource=resource.name, erase=True, key=_pack(key)))
        return self

    def commit(self):
        """Applies every operation or none; returns Applied or raises."""
        if self.result is not None:
            return self.result
        if not self._ops:
            raise InvalidRequest('empty transaction')
        self._client._check_bound(self._epoch, 'this transaction')
        request = v2.ApplyTransactionRequest(
            request_id=self.request_id, ops=self._ops,
            consistency=_SCOPE_SNAPSHOT if self._snapshot else _REFERENTIAL)
        if self._expected is not None:
            request.expected_generation = self._expected
        self.result = self._client._commit(request, self._epoch)
        return self.result

    def _resolve(self, resource):
        if not isinstance(resource, Resource):
            resource = self._client.resource(resource)
        self._client._check_bound(resource.daemon_epoch, 'resource %r' % resource.name)
        if self._epoch is None:
            self._epoch = resource.daemon_epoch
        elif resource.daemon_epoch != self._epoch:
            raise StaleResource('resource %r belongs to daemon epoch %d, this transaction to %d'
                                % (resource.name, resource.daemon_epoch, self._epoch))
        return resource

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if exc_type is None:
            self.commit()
        return False


def _check_type(resource, what, message, expected):
    actual = message.DESCRIPTOR.full_name
    if actual != expected:
        raise InvalidRequest('%s of %s must be %s, not %s' % (what, resource.name, expected, actual))


def _pack(message):
    packed = any_pb2.Any()
    packed.Pack(message)
    return packed


# -- desired state ------------------------------------------------------------------

class PipelineSnapshot:
    """The active pipeline (control_v2.Pipeline) and its generation."""

    def __init__(self, pipeline, generation):
        self.pipeline = pipeline
        self.generation = generation

    def __repr__(self):
        return 'PipelineSnapshot(generation=%d, modules=%d)' % (
            self.generation, len(self.pipeline.modules))


class PipelineBuilder:
    """Assembles a control_v2.Pipeline, empty or from a snapshot. It only
    builds the message: the daemon validates, diffs and plans it. Arguments
    (`arg`) are the module's or driver's own protobuf messages."""

    def __init__(self, base=None):
        self._p = v2.Pipeline()
        if base is not None:
            self._p.CopyFrom(base)

    def port(self, name, driver, arg=None, rx_queues=0, tx_queues=0, rx_queue_size=0,
             tx_queue_size=0):
        port = self._p.ports.add(name=name, driver=driver, num_rx_queues=rx_queues,
                                 num_tx_queues=tx_queues, rx_queue_size=rx_queue_size,
                                 tx_queue_size=tx_queue_size)
        if arg is not None:
            port.arg.Pack(arg)
        return self

    def module(self, name, mclass, arg=None):
        module = self._p.modules.add(name=name, mclass=mclass)
        if arg is not None:
            module.arg.Pack(arg)
        return self

    def connect(self, upstream, downstream, ogate=0, igate=0, skip_default_hooks=False):
        self._p.connections.add(upstream=upstream, ogate=ogate, downstream=downstream,
                                igate=igate, skip_default_hooks=skip_default_hooks)
        return self

    def chain(self, *names):
        """Gate 0 to gate 0 along `names`."""
        for upstream, downstream in zip(names, names[1:]):
            self.connect(upstream, downstream)
        return self

    def worker(self, wid, core, scheduler=''):
        self._p.workers.add(wid=wid, core=core, scheduler=scheduler)
        return self

    def traffic_class(self, name, policy, parent='', resource='', wid=-1, priority=None,
                      share=None, limit=None, max_burst=None, leaf_module_name='',
                      leaf_module_taskid=0):
        tc = self._p.traffic_classes.add(name=name, policy=policy, parent=parent,
                                         resource=resource, wid=wid,
                                         leaf_module_name=leaf_module_name,
                                         leaf_module_taskid=leaf_module_taskid)
        if priority is not None:
            tc.priority = priority
        if share is not None:
            tc.share = share
        tc.limit.update(limit or {})
        tc.max_burst.update(max_burst or {})
        return self

    def remove(self, name):
        """Drops the port, module or traffic class `name`, and the
        connections that touch it."""
        for field in (self._p.ports, self._p.modules, self._p.traffic_classes):
            for item in [x for x in field if x.name == name]:
                field.remove(item)
        for c in [c for c in self._p.connections if name in (c.upstream, c.downstream)]:
            self._p.connections.remove(c)
        return self

    def build(self):
        """A copy of the pipeline as built so far."""
        out = v2.Pipeline()
        out.CopyFrom(self._p)
        return out
