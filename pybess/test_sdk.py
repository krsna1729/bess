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
        self.listed = 0
        self.apply_script = list(apply)
        self.get_script = list(get)
        self.applied = []
        self.got = []

    def ListTransactionResources(self, request, timeout=None):
        self.listed += 1
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

    def test_supports_reads_the_daemons_rpcs_and_an_old_daemon_supports_nothing_new(self):
        class Capable(Stub):
            def GetCapabilities(self, request, timeout=None):
                return v2.GetCapabilitiesResponse(
                    rpcs=['bess.pb.v2.Control/ApplyTransaction'], daemon_epoch=self.epoch)

        class Old(Stub):
            def GetCapabilities(self, request, timeout=None):
                raise FakeRpcError(grpc.StatusCode.UNIMPLEMENTED, 'unknown method')

        capable = self.client(Capable())
        self.assertTrue(capable.supports('ApplyTransaction'))
        self.assertFalse(capable.supports('WatchEvents'))
        self.assertFalse(self.client(Old()).supports('ApplyTransaction'))

    def test_a_discovery_call_that_sees_a_restart_retires_old_handles(self):
        class Restarting(Stub):
            def GetCapabilities(self, request, timeout=None):
                return v2.GetCapabilitiesResponse(daemon_epoch=2)

        stub = Restarting()
        client = self.client(stub)
        old = client.resource(RESOURCE)  # epoch 1
        client.capabilities()            # the daemon now answers epoch 2
        stub.epoch = 2
        tx = client.transaction()
        with self.assertRaises(sdk.StaleResource):
            tx.upsert(old, KEY(request_id='k'), VALUE())
        self.assertEqual(stub.applied, [])

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

    def test_an_unanswered_question_is_asked_again_not_answered_by_resending(self):
        unavailable = FakeRpcError(grpc.StatusCode.UNAVAILABLE)
        stub = Stub(apply=[unavailable, (APPLIED, 1)], get=[unavailable, (False, 0, 1)])
        self.tx(self.client(stub)).commit()
        self.assertEqual(len(stub.got), 2, 'asked until it had an answer')
        self.assertEqual(len(stub.applied), 2)
        self.assertEqual(len({r.request_id for r in stub.applied}), 1)

    def test_a_restart_behind_a_failed_question_is_not_applied_blind(self):
        unavailable = FakeRpcError(grpc.StatusCode.UNAVAILABLE)
        stub = Stub(apply=[TIMEOUT], get=[unavailable, (False, 0, 2)])
        with self.assertRaises(sdk.DaemonRestarted):
            self.tx(self.client(stub)).commit()
        self.assertEqual(len(stub.applied), 1, 'never resent to the restarted daemon')

    def test_the_epoch_is_learned_before_the_first_send(self):
        stub = Stub(apply=[(APPLIED, 2)])
        client = self.client(stub)
        handle = sdk.Resource(RESOURCE, KEY.DESCRIPTOR.full_name, VALUE.DESCRIPTOR.full_name, 1)
        tx = client.transaction()
        tx.upsert(handle, KEY(), VALUE())  # no discovery through this client
        with self.assertRaises(sdk.DaemonRestarted):
            tx.commit()  # the daemon answered from epoch 2; it was 1 when the commit began
        self.assertEqual(stub.listed, 1)

    def test_a_handle_from_before_a_restart_is_refused_not_sent(self):
        stub = Stub(epoch=1, apply=[(APPLIED, 2)])
        client = self.client(stub)
        old = client.resource(RESOURCE)
        built = client.transaction()
        built.upsert(old, KEY(), VALUE())
        stub.epoch = 2
        client.resources(refresh=True)  # the client sees the restart
        with self.assertRaises(sdk.StaleResource):
            client.transaction().upsert(old, KEY(), VALUE())
        with self.assertRaises(sdk.StaleResource):
            built.commit()  # built before the restart: never sent
        self.assertEqual(stub.applied, [])
        fresh = client.resource(RESOURCE)  # looked up again: usable
        self.assertEqual(fresh.daemon_epoch, 2)
        result = client.transaction().upsert(fresh, KEY(), VALUE()).commit()
        self.assertIsInstance(result, sdk.Applied)
        self.assertEqual(len(stub.applied), 1)

    def test_a_restart_seen_by_a_commit_invalidates_the_old_handles(self):
        stub = Stub(epoch=1, apply=[(APPLIED, 2)])
        client = self.client(stub)
        old = client.resource(RESOURCE)
        with self.assertRaises(sdk.DaemonRestarted):
            client.transaction().upsert(old, KEY(), VALUE()).commit()
        with self.assertRaises(sdk.StaleResource):
            client.transaction().upsert(old, KEY(), VALUE())
        self.assertEqual(len(stub.applied), 1)

    def test_one_transaction_holds_handles_of_one_epoch(self):
        stub = Stub(epoch=1)
        client = self.client(stub)
        tx = client.transaction().upsert(client.resource(RESOURCE), KEY(), VALUE())
        stub.epoch = 2
        fresh = client.resources(refresh=True)[RESOURCE]  # the client sees the restart
        with self.assertRaises(sdk.StaleResource):
            tx.upsert(fresh, KEY(), VALUE())  # valid now, but the transaction is not

    def test_internal_without_bess_detail_is_no_answer(self):
        reset = FakeRpcError(grpc.StatusCode.INTERNAL, 'stream reset')
        stub = Stub(apply=[reset], get=[(True, APPLIED, 1)])
        result = self.tx(self.client(stub)).commit()
        self.assertTrue(result.replayed)
        detail = v2.ErrorDetail(code=v2.ErrorDetail.INTERNAL, message='engine failure')
        stub = Stub(apply=[FakeRpcError(grpc.StatusCode.INTERNAL, 'x', detail)])
        with self.assertRaises(sdk.InvalidRequest):
            self.tx(self.client(stub)).commit()

    def test_a_conflict_after_an_unanswered_send_says_it_may_have_applied(self):
        stub = Stub(apply=[TIMEOUT, (CONFLICT, 1)], get=[(False, 0, 1)])
        with self.assertRaises(sdk.Conflict) as raised:
            self.tx(self.client(stub), expected_generation=3).commit()
        self.assertTrue(raised.exception.after_unknown_attempt)
        stub = Stub(apply=[(CONFLICT, 1)])
        with self.assertRaises(sdk.Conflict) as raised:
            self.tx(self.client(stub), expected_generation=3).commit()
        self.assertFalse(raised.exception.after_unknown_attempt)

    def test_no_answer_at_all_is_an_unknown_outcome_not_a_failure(self):
        stub = Stub(apply=[TIMEOUT], get=[TIMEOUT])
        with self.assertRaises(sdk.TransportError) as raised:
            self.tx(self.client(stub, attempts=2)).commit()
        self.assertEqual(raised.exception.request_id, stub.applied[0].request_id)

    def test_out_of_attempts_after_an_unanswered_send_is_unknown_not_busy(self):
        cases = [
            (Stub(apply=[TIMEOUT], get=[(False, 0, 1)]), 2),
            (Stub(apply=[TIMEOUT, (BUSY, 1)], get=[(False, 0, 1)]), 3),
            (Stub(apply=[TIMEOUT, TIMEOUT], get=[(False, 0, 1), (False, 0, 1)]), 4),
        ]
        for stub, attempts in cases:
            with self.assertRaises(sdk.TransportError):
                self.tx(self.client(stub, attempts=attempts)).commit()

    def test_a_refused_question_after_an_unanswered_send_is_unknown(self):
        stub = Stub(apply=[TIMEOUT], get=[FakeRpcError(grpc.StatusCode.PERMISSION_DENIED, 'proxy')])
        with self.assertRaises(sdk.TransportError) as raised:
            self.tx(self.client(stub)).commit()
        self.assertEqual(raised.exception.cause, grpc.StatusCode.PERMISSION_DENIED)

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
