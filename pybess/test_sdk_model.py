# SPDX-License-Identifier: BSD-3-Clause
"""pybess.sdk's commit protocol against every small-scope behaviour of the
network and the daemon (roadmap Appendix L, A1+ for M27).

A simulated daemon keeps the truth: its epoch, and which request ids it
applied in which epoch. For every RPC the environment chooses what happens --
answered, request lost, reply lost after the daemon acted, busy -- and whether
the daemon restarts first (at most once). A depth-first search replays the
client against every sequence of choices and checks, for each outcome:

  - a request is applied at most once, over all epochs (no blind resend);
  - Applied only if the daemon applied it, in the epoch that answered;
  - Busy only if nothing was applied anywhere;
  - TransportError only after an RPC that went unanswered;
  - DaemonRestarted only if the daemon restarted.
"""

import unittest

import grpc

from . import sdk
from .test_sdk import FakeRpcError, KEY, RESOURCE, VALUE
from builtin_pb import control_v2_pb2 as v2

APPLIED = v2.TransactionRecord.OUTCOME_APPLIED
BUSY = v2.TransactionRecord.OUTCOME_BUSY
TIMEOUT = grpc.StatusCode.DEADLINE_EXCEEDED

# What the environment can do to one RPC.
APPLY_CHOICES = ('answer', 'lose_request', 'lose_reply', 'busy')
GET_CHOICES = ('answer', 'lose_request')


class Exhausted(Exception):
    """The client made more RPCs than the search bounds (a bug: it must not)."""


class Daemon:
    """The truth, and a stub that answers from it under scripted choices."""

    def __init__(self, choose, max_restarts=1):
        self.choose = choose
        self.epoch = 1
        self.restarts_left = max_restarts
        self.restarted = False
        self.applied = {}        # epoch -> set of request ids applied in it
        self.applications = 0    # every application, all epochs
        self.unanswered = False  # some RPC went without an answer

    def _maybe_restart(self):
        if self.restarts_left and self.choose(('stay', 'restart')) == 'restart':
            self.restarts_left -= 1
            self.restarted = True
            self.epoch += 1  # the new daemon remembers nothing

    def ListTransactionResources(self, request, timeout=None):
        return v2.ListTransactionResourcesResponse(
            resources=[v2.TransactionResource(name=RESOURCE, key_type=KEY.DESCRIPTOR.full_name,
                                              value_type=VALUE.DESCRIPTOR.full_name)],
            daemon_epoch=self.epoch)

    def ApplyTransaction(self, request, timeout=None):
        self._maybe_restart()
        what = self.choose(APPLY_CHOICES)
        if what == 'lose_request':
            self.unanswered = True
            raise FakeRpcError(TIMEOUT)
        if what == 'busy':
            return v2.ApplyTransactionResponse(
                record=v2.TransactionRecord(request_id=request.request_id, outcome=BUSY),
                daemon_epoch=self.epoch)
        seen = self.applied.setdefault(self.epoch, set())
        replayed = request.request_id in seen
        if not replayed:
            seen.add(request.request_id)
            self.applications += 1
        if what == 'lose_reply':
            self.unanswered = True
            raise FakeRpcError(TIMEOUT)
        return v2.ApplyTransactionResponse(
            record=v2.TransactionRecord(request_id=request.request_id, outcome=APPLIED,
                                        generation=1),
            replayed=replayed, daemon_epoch=self.epoch)

    def GetTransaction(self, request, timeout=None):
        self._maybe_restart()
        if self.choose(GET_CHOICES) == 'lose_request':
            self.unanswered = True
            raise FakeRpcError(TIMEOUT)
        known = request.request_id in self.applied.get(self.epoch, set())
        return v2.GetTransactionResponse(
            known=known,
            record=v2.TransactionRecord(request_id=request.request_id,
                                        outcome=APPLIED if known else 0, generation=1),
            daemon_epoch=self.epoch)


class Explorer:
    """Depth-first over the choice points: replays the run with a fixed
    prefix of choices, then takes the first option at each new point."""

    def __init__(self, max_choices):
        self.max_choices = max_choices

    def runs(self):
        prefix = []
        while True:
            taken = []

            def choose(options):
                if len(taken) >= self.max_choices:
                    raise Exhausted()
                pick = prefix[len(taken)] if len(taken) < len(prefix) else 0
                taken.append((pick, len(options)))
                return options[pick]

            yield choose
            # Next: advance the deepest point that still has options left.
            while taken and taken[-1][0] + 1 >= taken[-1][1]:
                taken.pop()
            if not taken:
                return
            prefix = [pick for pick, _ in taken[:-1]] + [taken[-1][0] + 1]


class SdkProtocolModelTest(unittest.TestCase):

    def check_all(self, attempts):
        paths = 0
        outcomes = {}
        for choose in Explorer(max_choices=4 * attempts + 4).runs():
            daemon = Daemon(choose)
            client = sdk.Client(stub=daemon, sleep=lambda _: None,
                                retry=sdk.RetryPolicy(attempts=attempts, busy_backoff=0))
            tx = client.transaction()
            tx.upsert(RESOURCE, KEY(request_id='k'), VALUE())
            try:
                result = tx.commit()
            except Exhausted:
                self.fail('the client made more RPCs than its attempt budget allows')
            except sdk.Error as error:
                result = error
            paths += 1
            kind = type(result).__name__
            outcomes[kind] = outcomes.get(kind, 0) + 1
            where = 'path %d: %s; daemon applied %d time(s), restarted=%s, unanswered=%s' % (
                paths, kind, daemon.applications, daemon.restarted, daemon.unanswered)
            self.assertLessEqual(daemon.applications, 1, 'applied more than once: ' + where)
            if isinstance(result, sdk.Applied):
                self.assertEqual(daemon.applications, 1, where)
                self.assertIn(tx.request_id, daemon.applied.get(result.daemon_epoch, set()), where)
            elif isinstance(result, sdk.Busy):
                self.assertEqual(daemon.applications, 0, where)
            elif isinstance(result, sdk.TransportError):
                self.assertTrue(daemon.unanswered, where)
            elif isinstance(result, sdk.DaemonRestarted):
                self.assertTrue(daemon.restarted, where)
            else:
                self.fail('unexpected outcome: ' + where)
        return paths, outcomes

    def test_every_behaviour_within_the_attempt_budget(self):
        previous = 0
        for attempts in (1, 2, 3, 4):
            paths, outcomes = self.check_all(attempts)
            # The search must have been broad and reached every outcome kind:
            # one attempt is exactly restart-or-not times the four Apply
            # behaviours; each more attempt explores strictly more.
            if attempts == 1:
                self.assertEqual(paths, 2 * len(APPLY_CHOICES), outcomes)
            self.assertGreater(paths, previous, outcomes)
            previous = paths
            if attempts >= 2:
                for kind in ('Applied', 'Busy', 'TransportError', 'DaemonRestarted'):
                    self.assertIn(kind, outcomes, (attempts, outcomes))


if __name__ == '__main__':
    unittest.main()
