# SPDX-License-Identifier: BSD-3-Clause

# The control SDK (pybess.sdk, M27) against a live daemon: resources are
# discovered with their types, a transaction steers packets, a request id is
# replayed rather than applied twice, and a mistyped key never leaves the
# client.

import socket
from test_utils import *
from builtin_pb import module_msg_pb2 as module_msg
from pybess import sdk


def rule_key(sip, dip):
    key = module_msg.ExactMatchRuleKey()
    key.fields.add(value_bin=socket.inet_aton(sip))
    key.fields.add(value_bin=socket.inet_aton(dip))
    return key


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
        self.assertTrue(any(p.driver == 'UnixSocketPort' and p.rx_queues == 1
                            for p in ports.values()), ports)
        samples = client.metrics()
        self.assertIn('bess_transaction_generation', {name for name, _ in samples})


suite = unittest.TestLoader().loadTestsFromTestCase(BessControlSdkTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
