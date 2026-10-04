# SPDX-License-Identifier: BSD-3-Clause
# Operational events (M25 phase 2, D-084) against a real bessd: a transaction
# becomes a "bess.transaction" event with its outcome and generation, sequences
# are gapless, and a stream that asks for what the daemon no longer holds gets
# a "bess.gap" first.

import socket
import threading
from test_utils import *
from builtin_pb import module_msg_pb2 as module_msg
from pybess import sdk


def rule_key(sip, dip):
    key = module_msg.ExactMatchRuleKey()
    key.fields.add(value_bin=socket.inet_aton(sip))
    key.fields.add(value_bin=socket.inet_aton(dip))
    return key


class BessEventsTest(BessModuleTestCase):

    def test_transactions_are_events(self):
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                {'offset': 30, 'num_bytes': 4}])
        client = sdk.Client(self.bess.peer)
        rules = client.resource(em.name + '/rules')
        seen = []
        ready = threading.Event()

        def watch():
            stream = client.watch_events(types=['bess.transaction'], reconnect=False)
            ready.set()
            for event in stream:
                seen.append(event)
                if len(seen) == 2:
                    return

        watcher = threading.Thread(target=watch, daemon=True)
        watcher.start()
        ready.wait(5)
        time.sleep(0.5)  # the stream is open before the transactions
        with client.transaction() as tx:
            tx.upsert(rules, rule_key('1.2.3.4', '5.6.7.8'), module_msg.ExactMatchRuleValue(gate=1))
        stale = client.transaction(expected_generation=tx.result.generation - 1)
        stale.upsert(rules, rule_key('1.2.3.4', '5.6.7.8'), module_msg.ExactMatchRuleValue(gate=2))
        with self.assertRaises(sdk.Conflict):
            stale.commit()
        watcher.join(10)
        self.assertEqual(len(seen), 2, seen)
        self.assertEqual([e.fields['outcome'] for e in seen], ['applied', 'conflict'])
        self.assertEqual(seen[0].generation, tx.result.generation)
        self.assertEqual(seen[0].fields['request_id'], tx.request_id)
        self.assertGreater(seen[1].sequence, seen[0].sequence)
        self.assertEqual(seen[0].daemon_epoch, client.daemon_epoch)

    def test_a_reader_from_the_start_gets_what_is_held(self):
        client = sdk.Client(self.bess.peer)
        events = client.watch_events(from_sequence=1, reconnect=False)
        first = next(events)
        # Sequence 1 is held (this daemon has emitted few events), or a gap
        # names what is not.
        self.assertTrue(first.sequence == 1 or first.type == 'bess.gap', first)


    def test_a_sequence_this_daemon_never_reached_is_a_restart(self):
        # A controller resuming from a sequence it saved before a restart:
        # told so, not left waiting for sequences that will mean other events.
        client = sdk.Client(self.bess.peer)
        client.resources(refresh=True)
        with self.assertRaises(sdk.DaemonRestarted):
            next(client.watch_events(from_sequence=10 ** 12, daemon_epoch=client.daemon_epoch,
                                     reconnect=False))


suite = unittest.TestLoader().loadTestsFromTestCase(BessEventsTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
