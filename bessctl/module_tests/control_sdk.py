# SPDX-License-Identifier: BSD-3-Clause

# The control SDK (pybess.sdk, M27) against a live daemon: resources are
# discovered with their types, a transaction steers packets, a request id is
# replayed rather than applied twice, and a mistyped key never leaves the
# client. Black-box recovery: an answer lost after the daemon applied is
# recovered through GetTransaction (applied once); a request lost before it
# arrived is sent again; a restart between the send and the question is
# DaemonRestarted, and the old handles are refused. Desired state: a pipeline
# built, planned and applied through the SDK, then a stale generation.

import os
import shlex
import socket
import subprocess
import time
import grpc
from test_utils import *
from builtin_pb import control_v2_pb2 as v2
from builtin_pb import control_v2_pb2_grpc as v2_grpc
from builtin_pb import module_msg_pb2 as module_msg
from pybess import sdk


def rule_key(sip, dip):
    key = module_msg.ExactMatchRuleKey()
    key.fields.add(value_bin=socket.inet_aton(sip))
    key.fields.add(value_bin=socket.inet_aton(dip))
    return key


class Lost(grpc.RpcError):
    """What a client sees when the answer (or the request) is lost."""

    def __init__(self, code=grpc.StatusCode.DEADLINE_EXCEEDED):
        super().__init__()
        self._code = code

    def code(self):
        return self._code

    def details(self):
        return 'lost'

    def trailing_metadata(self):
        return ()


class LossyStub:
    """The daemon's real stub, with ApplyTransaction's fate scripted per
    call: 'deliver', 'lose_answer' (sent, then the answer is lost, after
    `then()`), or 'lose_request' (never sent)."""

    def __init__(self, peer, fates, then=lambda: None):
        self._real = v2_grpc.ControlStub(grpc.insecure_channel(peer))
        self._fates = list(fates)
        self._then = then
        self.sent = 0

    def __getattr__(self, name):
        return getattr(self._real, name)

    def ApplyTransaction(self, request, timeout=None):
        fate = self._fates.pop(0) if self._fates else 'deliver'
        if fate == 'lose_request':
            raise Lost(grpc.StatusCode.UNAVAILABLE)
        self.sent += 1
        response = self._real.ApplyTransaction(request, timeout=timeout)
        if fate == 'lose_answer':
            self._then()
            raise Lost()
        return response


def restart_daemon():
    """A new bessd in place of the running one (-k), as bessctl starts it."""
    bessd = os.environ.get('BESSD_BINARY', os.path.join(
        os.path.dirname(os.path.abspath(sdk.__file__)), '..', 'core', 'bessd'))
    command = [bessd, '-k', '-m', '0']
    if os.environ.get('LD_LIBRARY_PATH'):
        command = ['env', 'LD_LIBRARY_PATH=' + os.environ['LD_LIBRARY_PATH']] + command
    subprocess.check_call('sudo -E ' + ' '.join(shlex.quote(a) for a in command), shell=True)


class BessControlSdkTest(BessModuleTestCase):

    def test_transactions_through_the_sdk(self):
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                {'offset': 30, 'num_bytes': 4}])
        em.set_default_gate(gate=3)
        client = sdk.Client(self.bess.peer)
        rules = client.resource(em.name + '/rules')
        self.assertEqual(rules.key_type, module_msg.ExactMatchRuleKey.DESCRIPTOR.full_name)
        self.assertEqual(rules.value_type, module_msg.ExactMatchRuleValue.DESCRIPTOR.full_name)

        pkt = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')
        key = rule_key('65.43.21.0', '12.34.56.78')
        with client.transaction() as tx:
            tx.upsert(rules, key, module_msg.ExactMatchRuleValue(gate=1))
        self.assertIsInstance(tx.result, sdk.Applied)
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[1]), 1)

        # The same request again (a client retrying after a lost answer) is
        # the recorded outcome, not a second application.
        again = client.transaction(request_id=tx.request_id)
        again.upsert(rules, key, module_msg.ExactMatchRuleValue(gate=1))
        replay = again.commit()
        self.assertTrue(replay.replayed)
        self.assertEqual(replay.generation, tx.result.generation)

        # A stale expected generation changes nothing.
        stale = client.transaction(expected_generation=tx.result.generation - 1)
        stale.upsert(rules, key, module_msg.ExactMatchRuleValue(gate=2))
        with self.assertRaises(sdk.Conflict):
            stale.commit()
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[1]), 1)

        # A key of the wrong type is refused before anything is sent.
        with self.assertRaises(sdk.InvalidRequest):
            client.transaction().upsert(rules, module_msg.ExactMatchRuleValue(), module_msg.ExactMatchRuleValue())

        with client.transaction() as tx:
            tx.erase(rules, key)
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[3]), 1)


    def test_a_lost_answer_is_recovered_and_a_lost_request_resent(self):
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                {'offset': 30, 'num_bytes': 4}])
        em.set_default_gate(gate=3)
        pkt = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')
        key = rule_key('65.43.21.0', '12.34.56.78')

        # Applied, answer lost: the client asks, and reports the recorded
        # outcome -- one application, not two.
        stub = LossyStub(self.bess.peer, ['lose_answer'])
        client = sdk.Client(stub=stub)
        rules = client.resource(em.name + '/rules')
        before = client.metrics()[('bess_transactions_total', (('outcome', 'applied'),))]
        tx = client.transaction()
        tx.upsert(rules, key, module_msg.ExactMatchRuleValue(gate=1))
        applied = tx.commit()
        self.assertTrue(applied.replayed)
        self.assertEqual(stub.sent, 1)
        after = client.metrics()[('bess_transactions_total', (('outcome', 'applied'),))]
        self.assertEqual(after - before, 1)
        self.assertEqual(len(self.run_module(em, 0, [pkt], [0, 1, 2, 3])[1]), 1)

        # Never arrived: "not known", so the same request is sent again.
        stub = LossyStub(self.bess.peer, ['lose_request'])
        client = sdk.Client(stub=stub)
        tx = client.transaction()
        tx.upsert(client.resource(em.name + '/rules'), key, module_msg.ExactMatchRuleValue(gate=2))
        applied = tx.commit()
        self.assertFalse(applied.replayed)
        self.assertEqual(stub.sent, 1)
        self.assertEqual(len(self.run_module(em, 0, [pkt], [0, 1, 2, 3])[2]), 1)

    def test_a_restart_between_send_and_question_is_daemon_restarted(self):
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                {'offset': 30, 'num_bytes': 4}])
        key = rule_key('65.43.21.0', '12.34.56.78')
        stub = LossyStub(self.bess.peer, ['lose_answer'], then=restart_daemon)
        client = sdk.Client(stub=stub, retry=sdk.RetryPolicy(attempt_timeout=10, attempts=6))
        rules = client.resource(em.name + '/rules')
        epoch = client.daemon_epoch
        tx = client.transaction()
        tx.upsert(rules, key, module_msg.ExactMatchRuleValue(gate=1))
        try:
            with self.assertRaises(sdk.DaemonRestarted):
                tx.commit()
        finally:
            # The harness's connections (this test's and the module
            # wrappers') were to the old daemon.
            for b in (self.bess, em.bess):
                b.disconnect()
                b.connect(grpc_url=self.bess.peer)
        self.assertNotEqual(client.daemon_epoch, epoch)
        # The old handle belongs to the lost daemon: refused, never sent.
        with self.assertRaises(sdk.StaleResource):
            client.transaction().upsert(rules, key, module_msg.ExactMatchRuleValue(gate=1))
        # The restarted daemon has no such module.
        with self.assertRaises(sdk.InvalidRequest):
            client.resource(em.name + '/rules')

    def test_a_pipeline_through_the_sdk(self):
        client = sdk.Client(self.bess.peer)
        snap = client.pipeline()
        built = (sdk.PipelineBuilder(snap.pipeline)
                 .module('sdk_a', 'Bypass').module('sdk_b', 'Sink').chain('sdk_a', 'sdk_b')
                 .build())
        steps, generation = client.plan_pipeline(built)
        self.assertEqual(generation, snap.generation)
        self.assertIn('sdk_b', [s.object for s in steps])
        done = client.apply_pipeline(built, expected_generation=snap.generation)
        self.assertGreater(done.generation, snap.generation)
        names = [m.name for m in client.pipeline().pipeline.modules]
        self.assertIn('sdk_a', names)
        # The same desired state again changes nothing.
        self.assertEqual(client.apply_pipeline(built).generation, done.generation)
        # Built against a generation that has moved on: nothing changes.
        removal = sdk.PipelineBuilder(built).remove('sdk_b').build()
        with self.assertRaises(sdk.Conflict):
            client.apply_pipeline(removal, expected_generation=snap.generation)
        self.assertIn('sdk_b', [m.name for m in client.pipeline().pipeline.modules])
        client.apply_pipeline(sdk.PipelineBuilder(removal).remove('sdk_a').build())
        self.assertNotIn('sdk_a', [m.name for m in client.pipeline().pipeline.modules])

    def test_capabilities(self):
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4}])
        client = sdk.Client(self.bess.peer)
        caps = client.capabilities()
        self.assertTrue(caps.daemon_version)
        self.assertIn('bess.pb.v2.Control/GetCapabilities', caps.rpcs)
        self.assertTrue(client.supports('ApplyTransaction'))
        self.assertFalse(client.supports('NoSuchRpc'))
        self.assertIn('ExactMatch', caps.module_classes)
        self.assertIn(em.name + '/rules', [r.name for r in caps.resources])
        self.assertEqual(caps.daemon_epoch, client.daemon_epoch)
        self.assertGreater(caps.plugin_api_version, 0)
        # The harness's unix socket ports appear once created.
        self.run_module(em, 0, [get_tcp_packet(sip='1.2.3.4', dip='5.6.7.8')], [0])
        ports = {p.name: p for p in client.capabilities().ports}
        self.assertTrue(any(p.driver == 'UnixSocketPort' and p.rx_queues == 1 and p.symmetric_rss
                            for p in ports.values()), ports)
        samples = client.metrics()
        self.assertIn('bess_transaction_generation', {name for name, _ in samples})


suite = unittest.TestLoader().loadTestsFromTestCase(BessControlSdkTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
