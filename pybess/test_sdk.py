# SPDX-License-Identifier: BSD-3-Clause
"""pybess.sdk against a scripted stub: every recovery path of a transaction."""

import unittest

import grpc

from . import sdk
from builtin_pb import control_v2_pb2 as v2

# Any two message types stand in for a resource's key and value.
KEY = v2.GetTransactionRequest
VALUE = v2.ListTransactionResourcesRequest
RESOURCE = 'm0/table'


class FakeRpcError(grpc.RpcError):

    def __init__(self, code, details='', detail=None):
        super().__init__()
        self._code = code
        self._details = details
        self._detail = detail

    def code(self):
        return self._code

    def details(self):
        return self._details

    def trailing_metadata(self):
        if self._detail is None:
            return ()
        return (('bess-error-bin', self._detail.SerializeToString()),)


def record(outcome, request_id='', generation=7):
    return v2.TransactionRecord(request_id=request_id, outcome=outcome, generation=generation)


class Stub:
    """Answers each RPC from a script: a list per method of responses or
    exceptions, consumed in order. Records every request."""

    def __init__(self, epoch=1, apply=(), get=()):
        self.epoch = epoch
        self.apply_script = list(apply)
        self.get_script = list(get)
        self.applied = []
        self.got = []

    def ListTransactionResources(self, request, timeout=None):
        return v2.ListTransactionResourcesResponse(
            resources=[v2.TransactionResource(name=RESOURCE, key_type=KEY.DESCRIPTOR.full_name,
                                              value_type=VALUE.DESCRIPTOR.full_name)],
            generation=7, daemon_epoch=self.epoch)

    def ApplyTransaction(self, request, timeout=None):
        sent = v2.ApplyTransactionRequest()
        sent.CopyFrom(request)  # what went on the wire, not the caller's object
        self.applied.append(sent)
        step = self.apply_script.pop(0)
        if isinstance(step, Exception):
            raise step
        outcome, epoch = step
        return v2.ApplyTransactionResponse(record=record(outcome, request.request_id),
                                           daemon_epoch=epoch)

    def GetTransaction(self, request, timeout=None):
        self.got.append(request.request_id)
        step = self.get_script.pop(0)
        if isinstance(step, Exception):
            raise step
        known, outcome, epoch = step
        return v2.GetTransactionResponse(known=known, record=record(outcome, request.request_id),
                                         daemon_epoch=epoch)


APPLIED = v2.TransactionRecord.OUTCOME_APPLIED
REJECTED = v2.TransactionRecord.OUTCOME_REJECTED
CONFLICT = v2.TransactionRecord.OUTCOME_CONFLICT
BUSY = v2.TransactionRecord.OUTCOME_BUSY
TIMEOUT = FakeRpcError(grpc.StatusCode.DEADLINE_EXCEEDED)


class SdkTest(unittest.TestCase):

    def client(self, stub, attempts=4):
        self.sleeps = []
        return sdk.Client(stub=stub, retry=sdk.RetryPolicy(attempts=attempts, busy_backoff=0.01),
                          sleep=self.sleeps.append)

    def tx(self, client, **kw):
        tx = client.transaction(**kw)
        tx.upsert(RESOURCE, KEY(request_id='k'), VALUE())
        return tx

    def test_types_are_checked_before_anything_is_sent(self):
        stub = Stub()
        client = self.client(stub)
        tx = client.transaction()
        with self.assertRaises(sdk.InvalidRequest):
            tx.upsert(RESOURCE, VALUE(), VALUE())
        with self.assertRaises(sdk.InvalidRequest):
            tx.upsert(RESOURCE, KEY(), KEY())
        with self.assertRaises(sdk.InvalidRequest):
            tx.upsert('missing/table', KEY(), VALUE())
        self.assertEqual(stub.applied, [])

    def test_applied(self):
        stub = Stub(apply=[(APPLIED, 1)])
        client = self.client(stub)
        result = self.tx(client).commit()
        self.assertIsInstance(result, sdk.Applied)
        self.assertEqual(result.generation, 7)
        self.assertFalse(result.replayed)
        self.assertEqual(len(stub.applied), 1)
        self.assertTrue(stub.applied[0].request_id)

    def test_a_timeout_is_resolved_by_asking_not_by_resending(self):
        stub = Stub(apply=[TIMEOUT], get=[(True, APPLIED, 1)])
        result = self.tx(self.client(stub)).commit()
        self.assertIsInstance(result, sdk.Applied)
        self.assertTrue(result.replayed)
        self.assertEqual(len(stub.applied), 1, 'it applied: no second send')
        self.assertEqual(stub.got, [stub.applied[0].request_id])

    def test_an_unseen_request_is_resent_identically(self):
        stub = Stub(apply=[TIMEOUT, (APPLIED, 1)], get=[(False, 0, 1)])
        result = self.tx(self.client(stub)).commit()
        self.assertIsInstance(result, sdk.Applied)
        self.assertEqual(len(stub.applied), 2)
        self.assertEqual(stub.applied[0], stub.applied[1], 'same id and contents')

    def test_an_unreachable_daemon_keeps_the_request_id(self):
        unavailable = FakeRpcError(grpc.StatusCode.UNAVAILABLE)
        stub = Stub(apply=[unavailable, unavailable, (APPLIED, 1)], get=[unavailable, (False, 0, 1)])
        self.tx(self.client(stub)).commit()
        self.assertEqual(len({r.request_id for r in stub.applied}), 1)

    def test_no_answer_at_all_is_an_unknown_outcome_not_a_failure(self):
        stub = Stub(apply=[TIMEOUT, TIMEOUT], get=[TIMEOUT, TIMEOUT])
        with self.assertRaises(sdk.TransportError) as raised:
            self.tx(self.client(stub, attempts=2)).commit()
        self.assertEqual(raised.exception.request_id, stub.applied[0].request_id)

    def test_a_restart_during_recovery_is_reported_not_guessed(self):
        stub = Stub(apply=[TIMEOUT], get=[(False, 0, 2)])
        with self.assertRaises(sdk.DaemonRestarted):
            self.tx(self.client(stub)).commit()
        self.assertEqual(len(stub.applied), 1, 'not resent to the new daemon')

    def test_busy_is_retried_with_backoff(self):
        stub = Stub(apply=[(BUSY, 1), (BUSY, 1), (APPLIED, 1)])
        result = self.tx(self.client(stub)).commit()
        self.assertIsInstance(result, sdk.Applied)
        self.assertEqual(self.sleeps, [0.01, 0.02])
        self.assertEqual(len({r.request_id for r in stub.applied}), 1)

    def test_busy_through_every_attempt(self):
        stub = Stub(apply=[(BUSY, 1)] * 3)
        with self.assertRaises(sdk.Busy):
            self.tx(self.client(stub, attempts=3)).commit()

    def test_conflict_and_rejection(self):
        stub = Stub(apply=[(CONFLICT, 1)])
        with self.assertRaises(sdk.Conflict):
            self.tx(self.client(stub), expected_generation=3).commit()
        self.assertEqual(stub.applied[0].expected_generation, 3)

        class RejectingStub(Stub):
            def ApplyTransaction(self, request, timeout=None):
                rec = record(REJECTED, request.request_id)
                rec.ops.add(status=v2.TransactionOpResult.STATUS_FAILED, error='no such next hop')
                return v2.ApplyTransactionResponse(record=rec, daemon_epoch=1)

        with self.assertRaises(sdk.Rejected) as raised:
            self.tx(self.client(RejectingStub())).commit()
        self.assertEqual(raised.exception.failures, [(0, 'no such next hop')])

    def test_server_errors_carry_their_detail(self):
        detail = v2.ErrorDetail(code=v2.ErrorDetail.INVALID_ARGUMENT, message='bad key', field='key')
        stub = Stub(apply=[FakeRpcError(grpc.StatusCode.INVALID_ARGUMENT, 'x', detail)])
        with self.assertRaises(sdk.InvalidRequest) as raised:
            self.tx(self.client(stub)).commit()
        self.assertEqual(raised.exception.detail.field, 'key')
        self.assertIn('bad key', str(raised.exception))

    def test_the_context_manager_commits_only_on_a_clean_exit(self):
        stub = Stub(apply=[(APPLIED, 1)])
        client = self.client(stub)
        with client.transaction() as tx:
            tx.upsert(RESOURCE, KEY(), VALUE())
        self.assertIsInstance(tx.result, sdk.Applied)
        with self.assertRaises(ValueError):
            with client.transaction() as tx:
                tx.upsert(RESOURCE, KEY(), VALUE())
                raise ValueError('application error')
        self.assertEqual(len(stub.applied), 1)


if __name__ == '__main__':
    unittest.main()
