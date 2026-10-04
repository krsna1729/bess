# SPDX-License-Identifier: BSD-3-Clause

# Operational metrics (M25) from a live daemon: ListMetrics carries the
# runtime's RCU and transaction-engine sources, transaction counters move with
# transactions, and the Prometheus exporter's output parses.

import os
import socket
import sys
from test_utils import *
from builtin_pb import control_v2_pb2 as control_v2
from builtin_pb import module_msg_pb2 as module_msg

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import bess_prometheus  # noqa: E402


def value(response, name, **labels):
    for s in response.samples:
        if s.name == name and all(s.labels.get(k) == v for k, v in labels.items()):
            return s.value
    raise AssertionError('no sample %s %s' % (name, labels))


class BessMetricsTest(BessModuleTestCase):

    def metrics(self):
        return self.bess.stub_v2.ListMetrics(control_v2.ListMetricsRequest())

    def test_runtime_metrics_move_with_transactions(self):
        before = self.metrics()
        for name in ('bess_rcu_pending_retired_objects', 'bess_rcu_grace_periods_started_total',
                     'bess_transaction_generation', 'bess_transaction_pending_cascades'):
            value(before, name)
        applied = value(before, 'bess_transactions_total', outcome='applied')
        rejected = value(before, 'bess_transactions_total', outcome='rejected')

        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4}, {'offset': 30, 'num_bytes': 4}])
        key = module_msg.ExactMatchRuleKey()
        key.fields.add(value_bin=socket.inet_aton('10.0.0.1'))
        key.fields.add(value_bin=socket.inet_aton('10.0.0.2'))
        op = self.bess.transaction_op(em.name + '/rules', key, module_msg.ExactMatchRuleValue(gate=1))
        self.bess.apply_transaction([op])
        # Erasing a key that is not there is refused: a rejection.
        self.bess.apply_transaction([self.bess.transaction_op(
            em.name + '/rules', module_msg.ExactMatchRuleKey(fields=[
                {'value_bin': socket.inet_aton('9.9.9.9')}, {'value_bin': socket.inet_aton('9.9.9.9')}]),
            erase=True)])

        after = self.metrics()
        self.assertEqual(value(after, 'bess_transactions_total', outcome='applied'), applied + 1)
        self.assertEqual(value(after, 'bess_transactions_total', outcome='rejected'), rejected + 1)
        self.assertGreater(value(after, 'bess_transaction_generation'),
                           value(before, 'bess_transaction_generation'))
        self.assertEqual(after.daemon_epoch, before.daemon_epoch)

        text = bess_prometheus.exposition(after)
        types = {}
        for line in text.splitlines():
            if line.startswith('# TYPE '):
                _, _, name, kind = line.split(' ')
                types[name] = kind
            elif line and not line.startswith('#'):
                name = line.split('{')[0].split(' ')[0]
                self.assertIn(name, types, 'a sample before its TYPE line')
                float(line.rsplit(' ', 1)[1])
        self.assertEqual(types['bess_transactions_total'], 'counter')
        self.assertEqual(types['bess_rcu_pending_retired_objects'], 'gauge')


suite = unittest.TestLoader().loadTestsFromTestCase(BessMetricsTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
