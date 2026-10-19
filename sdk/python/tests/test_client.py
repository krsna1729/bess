# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the Python SDK, on a scripted transport (D-146).

No daemon and no network: the transport is a list of scripted answers, so the
parts of the protocol that are easy to get wrong - the request id across
retries, recovery after a lost answer, the epoch, rejected operations - are
tested rather than assumed.
"""

import unittest

import grpc

from bess import errors
from bess.client import (
    COUNTER_THRESHOLD,
    NO_WORKER,
    Applied,
    Client,
    RetryPolicy,
    _translate,
)
from bess.controlv2 import control_v2_pb2 as v2
from bess.pipeline import PipelineBuilder, PipelineVersion, Resource


def rpc_error(detail=None, message="scripted failure"):
    """A grpc.RpcError carrying the daemon's ErrorDetail the way the wire does.

    The daemon attaches the typed detail as the `bess-error-bin` binary trailer
    (core/control/api_v2.cc:486), and `RpcError.details()` is the status
    message, not the detail. A fake that put the detail in `details()` would
    let a broken client pass, so this one carries the real trailer.
    """
    error = grpc.RpcError(message)
    if detail is None:
        error.trailing_metadata = lambda: ()
    else:
        payload = detail.SerializeToString()
        error.trailing_metadata = lambda: (("bess-error-bin", payload),)
    return error


class FakeTransport:
    """Plays a scripted sequence: an answer per call, or an exception."""

    def __init__(self, answers):
        self.answers = list(answers)
        self.requests = []
        self.methods = []

    def call(self, method, request, timeout=None, admin=False):
        self.methods.append(method)
        self.requests.append(request)
        if not self.answers:
            raise AssertionError(f"unexpected call: {method}")
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer

    def stream(self, method, request, timeout=None, admin=False):
        self.methods.append(method)
        self.requests.append(request)
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return iter(answer)

    def close(self):
        pass


def pipeline_response(generation=7, epoch=3):
    response = v2.GetPipelineResponse()
    response.generation = generation
    response.daemon_epoch = epoch
    return response


def transaction_response(
    request_id,
    generation=8,
    epoch=3,
    statuses=(v2.TransactionOpResult.STATUS_APPLIED,),
    outcome=v2.TransactionRecord.OUTCOME_APPLIED,
):
    response = v2.ApplyTransactionResponse()
    response.daemon_epoch = epoch
    response.record.request_id = request_id
    response.record.generation = generation
    response.record.outcome = outcome
    for status in statuses:
        entry = response.record.ops.add()
        entry.status = status
        entry.error = "" if status == v2.TransactionOpResult.STATUS_APPLIED else "refused"
    return response


class ScriptedTest(unittest.TestCase):
    def make_client(self, answers, retry=None, epoch=3, seed_resources=True):
        """A client on a scripted transport.

        `epoch` is what an earlier answer would have reported. Requests the
        daemon requires an epoch on take it from there, so a test that does not
        care about the epoch needs no pipeline read in its script; pass 0 for a
        client that has seen no answer yet.

        `seed_resources` installs the listing a live client would have read:
        upsert and erase check a message's type against the resource's, and
        that check must not need a call of its own.
        """
        transport = FakeTransport(answers)
        client = Client(transport=transport, retry=retry or RetryPolicy(), sleep=lambda _: None)
        client._epoch = epoch
        if seed_resources:
            client._resources = {
                "em0/rules": Resource(
                    "em0/rules",
                    "bess.pb.v2.GetPipelineRequest",
                    "bess.pb.v2.GetPipelineResponse",
                ),
            }
        return client, transport

    # -- the transaction discipline ---------------------------------------

    def test_one_request_id_survives_a_busy_retry(self):
        """A busy answer is retried as the same transaction, not a new one."""
        client, transport = self.make_client(
            [
                rpc_error(v2.ErrorDetail(code=v2.ErrorDetail.RESOURCE_BUSY)),
                transaction_response("ignored"),
            ]
        )
        tx = client.transaction(snapshot=False)
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        client._transport.answers[1].record.request_id = tx.request_id
        applied = tx.commit()

        self.assertIsInstance(applied, Applied)
        self.assertEqual(["ApplyTransaction", "ApplyTransaction"], transport.methods)
        self.assertEqual(tx.request_id, transport.requests[0].request_id)
        self.assertEqual(tx.request_id, transport.requests[1].request_id)

    def test_busy_exhausts_the_attempt_budget(self):
        busy = rpc_error(v2.ErrorDetail(code=v2.ErrorDetail.RESOURCE_BUSY))
        client, _ = self.make_client([busy, rpc_error(v2.ErrorDetail(code=v2.ErrorDetail.RESOURCE_BUSY))],
                                     retry=RetryPolicy(attempts=2))
        tx = client.transaction(snapshot=False)
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        with self.assertRaises(errors.BusyError) as caught:
            tx.commit()
        self.assertEqual(2, caught.exception.attempts)

    def test_a_lost_answer_keeps_the_request_id_for_recovery(self):
        """The transport error names the request id, so GetTransaction can answer."""
        client, transport = self.make_client(
            [
                rpc_error(),
                v2.GetTransactionResponse(),
            ]
        )
        tx = client.transaction(snapshot=False)
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        with self.assertRaises(errors.TransportError) as caught:
            tx.commit()
        self.assertEqual(tx.request_id, caught.exception.request_id)

        client.get_transaction(caught.exception.request_id)
        self.assertEqual("GetTransaction", transport.methods[-1])
        self.assertEqual(tx.request_id, transport.requests[-1].request_id)

    def test_an_epoch_mismatch_is_reported_as_a_restart(self):
        detail = v2.ErrorDetail(code=v2.ErrorDetail.EPOCH_MISMATCH, message="epoch moved")
        self.assertIsInstance(_translate(rpc_error(detail)), errors.DaemonRestartedError)

    def test_a_rejected_transaction_lists_the_failing_operations(self):
        rejected = transaction_response(
            "id",
            statuses=(v2.TransactionOpResult.STATUS_APPLIED, v2.TransactionOpResult.STATUS_FAILED),
            outcome=v2.TransactionRecord.OUTCOME_REJECTED,
        )
        client, _ = self.make_client([rejected])
        tx = client.transaction(snapshot=False)
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        tx.erase("em0/rules", v2.GetPipelineRequest())
        with self.assertRaises(errors.RejectedError) as caught:
            tx.commit()
        self.assertEqual([1], [f.index for f in caught.exception.failures])
        self.assertEqual("refused", caught.exception.failures[0].error)

    def test_a_transaction_needs_an_operation(self):
        client, _ = self.make_client([])
        with self.assertRaises(errors.InvalidRequestError):
            client.transaction(snapshot=False).commit()

    # -- desired state -----------------------------------------------------

    def test_apply_names_the_last_epoch_it_saw(self):
        client, transport = self.make_client([pipeline_response(epoch=5), v2.ApplyPipelineResponse()])
        client.apply_pipeline(v2.Pipeline())
        self.assertEqual(["GetPipeline", "ApplyPipeline"], transport.methods)
        self.assertEqual(5, transport.requests[-1].daemon_epoch)

    def test_apply_with_an_expected_version_sends_both_fields(self):
        client, transport = self.make_client([v2.ApplyPipelineResponse()])
        client.apply_pipeline(v2.Pipeline(), expected=PipelineVersion(generation=11, daemon_epoch=2))
        self.assertEqual(11, transport.requests[-1].expected_generation)
        self.assertEqual(2, transport.requests[-1].daemon_epoch)

    def test_an_unknown_resource_is_a_client_side_error(self):
        listing = v2.ListTransactionResourcesResponse()
        entry = listing.resources.add()
        entry.name = "em0/rules"
        entry.key_type = "bess.pb.ExactMatchKey"
        entry.value_type = "bess.pb.ExactMatchValue"
        client, _ = self.make_client([listing, listing, listing], seed_resources=False)
        self.assertEqual("bess.pb.ExactMatchKey", client.resource("em0/rules").key_type)
        with self.assertRaises(errors.InvalidRequestError):
            client.resource("em0/nope")

    def test_supports_reads_full_method_names(self):
        """The daemon lists full method names; supports() is a membership test.

        The live daemon sends strings here (`repeated string rpcs`), so a
        version that treated them as objects with .name/.supported would raise
        against a real daemon while passing a fake built from the same wrong
        guess.
        """
        caps = v2.GetCapabilitiesResponse()
        caps.rpcs.extend(["bess.pb.v2.Control/ApplyTransaction",
                          "bess.pb.v2.Control/GetSchema"])
        client, _ = self.make_client([caps, caps])
        self.assertTrue(client.supports("bess.pb.v2.Control/ApplyTransaction"))
        self.assertFalse(client.supports("bess.pb.v2.Control/NoSuchRpc"))

    # -- the builder -------------------------------------------------------

    def test_the_builder_adds_connects_and_removes(self):
        built = (
            PipelineBuilder()
            .module("src", "Source")
            .module("sink0", "Sink")
            .connect("src", "sink0")
            .worker(0, 1)
            .build()
        )
        self.assertEqual(["src", "sink0"], [m.name for m in built.modules])
        self.assertEqual(1, len(built.connections))
        self.assertEqual("src", built.connections[0].upstream)

        trimmed = PipelineBuilder(built).remove("sink0").build()
        self.assertEqual(["src"], [m.name for m in trimmed.modules])
        self.assertEqual(0, len(trimmed.connections))
        self.assertEqual(2, len(built.modules), "build() returns a copy, so removing from one does not touch the other")

    def test_a_snapshot_carries_the_version(self):
        client, _ = self.make_client([pipeline_response(generation=4, epoch=9)])
        snapshot = client.get_pipeline()
        self.assertEqual(PipelineVersion(generation=4, daemon_epoch=9), snapshot.version)

    def test_a_conflict_record_is_not_a_success(self):
        """The record's outcome decides, not the operation statuses.

        A CONFLICT record reports no applied operation at all ("nothing
        tried"), so a client reading only the statuses would return Applied
        for a transaction that never ran.
        """
        client, _ = self.make_client([
            transaction_response("req-1", outcome=v2.TransactionRecord.OUTCOME_CONFLICT),
        ])
        tx = client.transaction(snapshot=False, request_id="req-1")
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        with self.assertRaises(errors.ConflictError):
            tx.commit()

    def test_a_busy_record_is_not_a_success(self):
        client, _ = self.make_client([
            transaction_response("req-1", outcome=v2.TransactionRecord.OUTCOME_BUSY),
        ], retry=RetryPolicy(attempts=1))
        tx = client.transaction(snapshot=False, request_id="req-1")
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        with self.assertRaises(errors.BusyError):
            tx.commit()

    def test_a_transaction_requires_the_dataplane_generation(self):
        """expected_generation is the dataplane generation, not the pipeline's.

        control_v2.proto: "Refuse (outcome OUTCOME_CONFLICT) unless the
        dataplane transaction generation is still this". Reading the pipeline
        generation here would conflict on every transaction once the two
        diverge.
        """
        snapshot_response = pipeline_response(generation=7, epoch=3)
        snapshot_response.resource_generation = 11
        client, transport = self.make_client([
            snapshot_response,
            transaction_response("req-1"),
        ])
        tx = client.transaction(request_id="req-1")
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        tx.commit()
        self.assertEqual(11, transport.requests[-1].expected_generation)
        self.assertEqual(3, transport.requests[-1].daemon_epoch)

    def test_get_transaction_reports_an_unknown_id(self):
        response = v2.GetTransactionResponse()
        response.known = False
        response.daemon_epoch = 3
        client, _ = self.make_client([response])
        known, record, epoch = client.get_transaction("never-sent")
        self.assertFalse(known)
        self.assertEqual(3, epoch)
        self.assertEqual("", record.request_id)

    def test_validate_reads_the_normalized_pipeline(self):
        response = v2.ValidatePipelineResponse()
        response.normalized.modules.add().name = "em0"
        client, _ = self.make_client([response])
        normalized = client.validate_pipeline(v2.Pipeline())
        self.assertEqual(["em0"], [m.name for m in normalized.modules])

    def test_admin_calls_send_the_epoch_of_the_last_answer(self):
        """Admin requests carry an epoch; the daemon refuses them without one."""
        client, transport = self.make_client([
            v2.PauseWorkersResponse(),
            v2.ResumeWorkersResponse(),
        ], epoch=5)
        client.pause_workers()
        client.resume_workers()
        self.assertEqual(5, transport.requests[0].daemon_epoch)
        self.assertEqual(5, transport.requests[1].daemon_epoch)

    def test_a_cold_client_reads_an_epoch_before_a_required_call(self):
        """An epoch-carrying request from a client that has seen no answer."""
        client, transport = self.make_client(
            [pipeline_response(epoch=7), v2.PauseWorkersResponse()], epoch=0)
        client.pause_workers()
        self.assertEqual(["GetPipeline", "PauseWorkers"], transport.methods)
        self.assertEqual(7, transport.requests[-1].daemon_epoch)

    def test_a_busy_record_is_retried_as_the_same_transaction(self):
        """The proto asks for a back-off and retry on OUTCOME_BUSY.

        Nothing was tried, so the retry is the same request id - and the same
        epoch, since the transaction was built under it.
        """
        client, transport = self.make_client([
            transaction_response("req-1", outcome=v2.TransactionRecord.OUTCOME_BUSY),
            transaction_response("req-1"),
        ], epoch=5)
        tx = client.transaction(snapshot=False, request_id="req-1")
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        applied = tx.commit()
        self.assertIsInstance(applied, Applied)
        self.assertEqual(["ApplyTransaction", "ApplyTransaction"], transport.methods)
        self.assertEqual(tx.request_id, transport.requests[1].request_id)
        self.assertEqual([5, 5], [r.daemon_epoch for r in transport.requests])

    def test_a_resource_added_after_a_listing_is_found(self):
        """A miss re-reads the listing, so a module an apply added is usable."""
        empty = v2.ListTransactionResourcesResponse()
        later = v2.ListTransactionResourcesResponse()
        entry = later.resources.add()
        entry.name = "em0/rules"
        entry.key_type = "bess.pb.v2.GetPipelineRequest"
        entry.value_type = "bess.pb.v2.GetPipelineResponse"
        client, transport = self.make_client([empty, empty, later], seed_resources=False)

        # The first upsert misses twice - the listing and the re-read - and is
        # refused; the second reads the refreshed listing and finds the name.
        with self.assertRaises(errors.InvalidRequestError):
            client.transaction(snapshot=False).upsert(
                "em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        tx = client.transaction(snapshot=False)
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        self.assertEqual(["ListTransactionResources"] * 3, transport.methods)

    def test_an_apply_drops_the_resource_cache(self):
        """A module an apply adds or removes changes the listing."""
        listing = v2.ListTransactionResourcesResponse()
        client, transport = self.make_client(
            [listing, pipeline_response(), v2.ApplyPipelineResponse(), listing],
            seed_resources=False)
        client.resources()
        client.apply_pipeline(v2.Pipeline())
        client.resources()
        self.assertEqual(2, transport.methods.count("ListTransactionResources"))

    def test_a_transaction_keeps_the_epoch_it_was_built_under(self):
        """A re-commit after a restart must not slip past EPOCH_MISMATCH."""
        client, transport = self.make_client(
            [transaction_response("req-1"), transaction_response("req-1")], epoch=5)
        tx = client.transaction(snapshot=False, request_id="req-1")
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        tx.commit()
        client._epoch = 9  # the daemon restarted between the two commits
        tx.commit()
        self.assertEqual([5, 5], [r.daemon_epoch for r in transport.requests])

    def test_a_mismatched_type_is_a_client_side_error(self):
        """A key or value whose type is not the resource's never reaches the wire.

        The daemon's listing names each resource's key and value types, so the
        check is a client-side error and not a round trip.
        """
        client, transport = self.make_client([])
        tx = client.transaction(snapshot=False)
        with self.assertRaises(errors.InvalidRequestError):
            tx.upsert("em0/rules", v2.GetPipelineResponse(), v2.GetPipelineResponse())
        with self.assertRaises(errors.InvalidRequestError):
            tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineRequest())
        self.assertEqual([], transport.methods, "nothing was sent")

    def test_a_watch_deadline_ends_the_stream(self):
        """A deadline the caller set is how a watch ends, not a failure."""
        deadline = grpc.RpcError("deadline")
        deadline.code = lambda: grpc.StatusCode.DEADLINE_EXCEEDED
        client, _ = self.make_client([deadline])
        self.assertEqual([], list(client.watch_events(timeout=1)))

    def test_a_snapshotless_transaction_still_sends_the_epoch(self):
        """daemon_epoch is required on a transaction: zero is INVALID_ARGUMENT.

        A transaction built without a snapshot has no version to take it from,
        so it must take the one the last answer reported.
        """
        client, transport = self.make_client(
            [pipeline_response(epoch=5), transaction_response("req-1")], epoch=0)
        tx = client.transaction(snapshot=False, request_id="req-1")
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        tx.commit()
        self.assertEqual(["GetPipeline", "ApplyTransaction"], transport.methods)
        self.assertEqual(5, transport.requests[-1].daemon_epoch)

    def test_invoke_action_sends_the_epoch(self):
        """InvokeAction is a control call, not an admin one, and needs it too."""
        client, transport = self.make_client(
            [pipeline_response(epoch=5), v2.InvokeActionResponse()], epoch=0)
        client.invoke_action("em0", "clear")
        self.assertEqual(5, transport.requests[-1].daemon_epoch)

    def test_a_watch_failure_is_not_swallowed(self):
        failure = rpc_error(message="boom")
        failure.code = lambda: grpc.StatusCode.UNAVAILABLE
        client, _ = self.make_client([failure])
        with self.assertRaises(errors.Error):
            list(client.watch_events(timeout=1))

    def test_a_watch_resumes_from_a_sequence(self):
        """from_sequence and daemon_epoch go on the request, so a stream resumes."""
        client, transport = self.make_client([[v2.Event()]])
        self.assertEqual(1, len(list(client.watch_events(from_sequence=7, daemon_epoch=3))))
        self.assertEqual(7, transport.requests[-1].from_sequence)
        self.assertEqual(3, transport.requests[-1].daemon_epoch)

    def test_a_skipped_operation_is_not_a_failure(self):
        """Only STATUS_FAILED caused the rejection; NOT_APPLIED was skipped."""
        record = transaction_response(
            "id",
            statuses=(v2.TransactionOpResult.STATUS_FAILED,
                      v2.TransactionOpResult.STATUS_NOT_APPLIED),
            outcome=v2.TransactionRecord.OUTCOME_REJECTED,
        )
        client, _ = self.make_client([record], retry=RetryPolicy(attempts=1))
        tx = client.transaction(snapshot=False)
        tx.upsert("em0/rules", v2.GetPipelineRequest(), v2.GetPipelineResponse())
        tx.erase("em0/rules", v2.GetPipelineRequest())
        with self.assertRaises(errors.RejectedError) as caught:
            tx.commit()
        self.assertEqual([0], [f.index for f in caught.exception.failures])


    def test_watch_counters_delivers_one_modules_crossings_typed(self):
        """Only the named module's, with the text fields parsed into numbers."""
        client, transport = self.make_client([[
            counter_event("other", 1, key="1", value="2", limit="3"),
            counter_event("usage", 2, key="7", value="100", limit="100"),
            counter_event("usage", 3, key="9", value="50", limit="50", worker="3"),
        ]])
        seen = list(client.watch_counters("usage"))
        self.assertEqual(2, len(seen), "another module's crossing was delivered")
        self.assertEqual(("usage", 7, 100, 100, NO_WORKER, 2, 20), tuple(seen[0]))
        self.assertEqual(3, seen[1].worker, "a worker's own plane reported it")
        self.assertEqual([COUNTER_THRESHOLD], list(transport.requests[-1].types))

    def test_watch_counters_raises_on_a_crossing_it_cannot_parse(self):
        """A producer that omits a field is reported, not skipped silently."""
        client, _ = self.make_client([[counter_event("usage", 2, key="7", limit="100")]])
        with self.assertRaises(ValueError) as caught:
            list(client.watch_counters("usage"))
        self.assertIn("value", str(caught.exception))

    def test_watch_counters_raises_on_a_value_that_is_not_a_number(self):
        """int() takes "1_000", " 7 " and "-5" into a uint64; Go's ParseUint does
        not, so the two SDKs must reject the same events."""
        client, _ = self.make_client([[
            counter_event("usage", 2, key="7", value="1_000", limit="100"),
        ]])
        with self.assertRaises(ValueError) as caught:
            list(client.watch_counters("usage"))
        self.assertIn("1_000", str(caught.exception))

    def test_watch_counters_raises_on_a_worker_above_what_an_int_holds(self):
        """Go's Worker is an int, so a worker that does not fit one is refused
        rather than narrowed (CodeQL found the narrowing in the Go parse)."""
        client, _ = self.make_client([[
            counter_event("usage", 2, key="7", value="100", limit="100", worker="4294967295"),
        ]])
        with self.assertRaises(ValueError) as caught:
            list(client.watch_counters("usage"))
        self.assertIn("4294967295", str(caught.exception))

    def test_watch_counters_draws_its_bounds_where_go_does(self):
        """The worker bound is an int's, the key/value/limit bound is 64 bits,
        and the check is strict, so the largest accepted worker passes."""
        client, _ = self.make_client([[
            counter_event("usage", 2, key="7", value="100", limit="100",
                          worker="2147483647"),
        ]])
        self.assertEqual([2**31 - 1], [c.worker for c in client.watch_counters("usage")])

        for field, value in (
            ("worker", "2147483648"),
            ("key", "18446744073709551616"),
            ("value", "18446744073709551616"),
            ("limit", "18446744073709551616"),
        ):
            fields = {"key": "7", "value": "100", "limit": "100", field: value}
            client, _ = self.make_client([[counter_event("usage", 2, **fields)]])
            with self.assertRaises(ValueError) as caught:
                list(client.watch_counters("usage"))
            self.assertIn(value, str(caught.exception), field)

    def test_watch_counters_raises_on_a_gap_in_the_log(self):
        """A gap may have held one of the module's crossings: say so, don't hide it."""
        client, _ = self.make_client([[
            counter_event("usage", 2, key="7", value="100", limit="100"),
            v2.Event(type="bess.gap", gap_from=3, gap_to=9, daemon_epoch=1),
        ]])
        seen = []
        with self.assertRaises(errors.GapError) as caught:
            for crossing in client.watch_counters("usage"):
                seen.append(crossing)
        self.assertEqual(1, len(seen), "the crossings before the gap are delivered")
        self.assertEqual("usage", caught.exception.module)
        self.assertEqual((3, 9), (caught.exception.gap_from, caught.exception.gap_to))

    def test_watch_counters_raises_on_a_restart(self):
        """watch_events yields the restart raw; the helper must not pass it over."""
        client, _ = self.make_client([[v2.Event(type="bess.restart", daemon_epoch=2)]])
        with self.assertRaises(errors.DaemonRestartedError):
            list(client.watch_counters("usage"))

    def test_watch_counters_passes_over_a_workers_lost_events(self):
        """A worker's ring loss names a worker, not a module: the watch goes on."""
        client, _ = self.make_client([[
            counter_event("usage", 2, key="7", value="100", limit="100"),
            v2.Event(type="bess.events_lost", sequence=3, source="worker1", daemon_epoch=1),
            counter_event("usage", 4, key="9", value="50", limit="50"),
            v2.Event(type="bess.restart", daemon_epoch=2),
        ]])
        seen = []
        with self.assertRaises(errors.DaemonRestartedError):
            for crossing in client.watch_counters("usage"):
                seen.append(crossing)
        self.assertEqual([7, 9], [c.key for c in seen])

    def test_watch_counters_forwards_the_resume_arguments(self):
        """from_sequence and daemon_epoch reach the request, as in watch_events."""
        client, transport = self.make_client([[]])
        list(client.watch_counters("usage", from_sequence=11, daemon_epoch=2))
        self.assertEqual(11, transport.requests[-1].from_sequence)
        self.assertEqual(2, transport.requests[-1].daemon_epoch)


def counter_event(source, sequence, **fields):
    """The event both producers emit: key, value and limit always, worker when
    a worker's own plane crossed (D-151, D-152)."""
    return v2.Event(
        sequence=sequence,
        type=COUNTER_THRESHOLD,
        source=source,
        daemon_epoch=1,
        time_ns=sequence * 10,
        fields=fields,
    )


if __name__ == "__main__":
    unittest.main()
