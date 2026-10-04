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
ErrorDetail). Conflicts and rejections are never retried.

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

    def __init__(self, message, detail=None):
        super().__init__(message)
        self.detail = detail  # control_v2.ErrorDetail, when the server sent one


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


class DaemonRestarted(Error):
    """The daemon restarted: its state, and the transaction's outcome, are
    gone. Re-read the state and decide again."""


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
    """A transactional resource and its key and value message types."""

    def __init__(self, name, key_type, value_type):
        self.name = name
        self.key_type = key_type
        self.value_type = value_type

    def __repr__(self):
        return 'Resource(%r, key=%s, value=%s)' % (self.name, self.key_type, self.value_type)


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
            self._resources = {r.name: Resource(r.name, r.key_type, r.value_type)
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

    def _observe_epoch(self, epoch):
        changed = self.daemon_epoch is not None and epoch != self.daemon_epoch
        self.daemon_epoch = epoch
        if changed:
            self._resources = None  # the restarted daemon's modules may differ
        return changed

    def _commit(self, request):
        """Applies `request` exactly once, whatever the transport does.

        Every RPC counts against the attempt budget. After a send without an
        answer the outcome is asked for, never assumed; the request is sent
        again only once GetTransaction has answered "not known" under the
        epoch this commit started with. An answer from any other epoch is
        DaemonRestarted: the transaction met a daemon that lost the state
        the caller built it against.
        """
        if self.daemon_epoch is None:
            self.resources(refresh=True)  # learn the epoch before sending
        epoch = self.daemon_epoch
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


def _translate(error):
    """A failed call's gRPC error as an SDK error (ErrorDetail decoded)."""
    detail = None
    for key, value in (error.trailing_metadata() or ()):
        if key == 'bess-error-bin':
            detail = v2.ErrorDetail()
            detail.ParseFromString(value)
    message = detail.message if detail is not None and detail.message else error.details()
    if detail is not None and detail.code == v2.ErrorDetail.CONFLICT:
        return InvalidRequest('request id reused with different contents: ' + message, detail)
    return InvalidRequest(message, detail)


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
        request = v2.ApplyTransactionRequest(
            request_id=self.request_id, ops=self._ops,
            consistency=_SCOPE_SNAPSHOT if self._snapshot else _REFERENTIAL)
        if self._expected is not None:
            request.expected_generation = self._expected
        self.result = self._client._commit(request)
        return self.result

    def _resolve(self, resource):
        return resource if isinstance(resource, Resource) else self._client.resource(resource)

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
