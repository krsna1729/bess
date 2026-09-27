# Copyright (c) 2026, Nefeli Networks, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# * Neither the names of the copyright holders nor the names of their
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# Dataplane transactions (G1.2c, D-025) against a live daemon: typed rule
# changes over the v2 RPC steer packets, apply across modules all or nothing,
# replay safely by request_id, and run while workers forward traffic.

import socket
from test_utils import *
from builtin_pb import control_v2_pb2 as control_v2
from builtin_pb import module_msg_pb2 as module_msg

APPLIED = control_v2.TransactionRecord.OUTCOME_APPLIED
REJECTED = control_v2.TransactionRecord.OUTCOME_REJECTED
BUSY = control_v2.TransactionRecord.OUTCOME_BUSY


def exact_rule(bess, module, sip, dip, gate=None):
    key = module_msg.ExactMatchRuleKey()
    key.fields.add(value_bin=socket.inet_aton(sip))
    key.fields.add(value_bin=socket.inet_aton(dip))
    if gate is None:
        return bess.transaction_op(module.name + '/rules', key, erase=True)
    return bess.transaction_op(module.name + '/rules', key,
                               module_msg.ExactMatchRuleValue(gate=gate))


def wild_rule(bess, module, sip, mask, priority, gate):
    key = module_msg.WildcardMatchRuleKey()
    key.values.add(value_bin=socket.inet_aton(sip))
    key.values.add(value_bin=b'\x00\x00\x00\x00')
    key.masks.add(value_bin=socket.inet_aton(mask))
    key.masks.add(value_bin=b'\x00\x00\x00\x00')
    return bess.transaction_op(
        module.name + '/rules', key,
        module_msg.WildcardMatchRuleValue(priority=priority, gate=gate))


def ip_fields():
    return [{'offset': 26, 'num_bytes': 4}, {'offset': 30, 'num_bytes': 4}]


class BessDataplaneTransactionsTest(BessModuleTestCase):

    def test_transaction_steers_packets(self):
        em = ExactMatch(fields=ip_fields())
        em.set_default_gate(gate=3)
        pkt = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')

        r = self.bess.apply_transaction(
            [exact_rule(self.bess, em, '65.43.21.0', '12.34.56.78', 1)])
        self.assertEqual(r.record.outcome, APPLIED)
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[1]), 1)
        self.assertSamePackets(outs[1][0], pkt)

        r = self.bess.apply_transaction(
            [exact_rule(self.bess, em, '65.43.21.0', '12.34.56.78')])
        self.assertEqual(r.record.outcome, APPLIED)
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[3]), 1)

    def test_across_modules_and_retries(self):
        em = ExactMatch(fields=ip_fields())
        wm = WildcardMatch(fields=ip_fields())
        names = [res.name for res in
                 self.bess.list_transaction_resources().resources]
        self.assertIn(em.name + '/rules', names)
        self.assertIn(wm.name + '/rules', names)

        ops = [exact_rule(self.bess, em, '10.0.0.1', '10.0.0.2', 1),
               wild_rule(self.bess, wm, '10.0.0.0', '255.0.0.0', 1, 7000)]
        r = self.bess.apply_transaction(ops, request_id='t-1')
        self.assertEqual(r.record.outcome, APPLIED)
        self.assertFalse(r.replayed)
        again = self.bess.apply_transaction(ops, request_id='t-1')
        self.assertTrue(again.replayed)
        self.assertEqual(again.record.generation, r.record.generation)
        known = self.bess.get_transaction('t-1')
        self.assertTrue(known.known)
        self.assertEqual(known.daemon_epoch, r.daemon_epoch)

        # One bad gate rejects both modules' changes.
        bad = [exact_rule(self.bess, em, '10.0.0.3', '10.0.0.4', 2),
               wild_rule(self.bess, wm, '11.0.0.0', '255.0.0.0', 1, 9000)]
        r = self.bess.apply_transaction(bad)
        self.assertEqual(r.record.outcome, REJECTED)
        self.assertIn('invalid gate', r.record.ops[1].error)
        self.assertEqual(len(em.get_runtime_config().rules), 1)
        self.assertEqual(len(wm.get_runtime_config().rules), 1)

    def test_transactions_while_traffic_runs(self):
        em = ExactMatch(fields=ip_fields())
        em.set_default_gate(gate=0)
        stats = {'applied': 0, 'busy': 0}

        def churn(i):
            # Add a rule for session i, remove session i - 16's.
            ops = [exact_rule(self.bess, em, '10.%d.%d.1' % (i // 256 % 256,
                                                              i % 256),
                              '10.0.0.2', 1 + i % 3)]
            if i >= 16:
                j = i - 16
                ops.append(exact_rule(self.bess, em,
                                      '10.%d.%d.1' % (j // 256 % 256,
                                                      j % 256), '10.0.0.2'))
            r = self.bess.apply_transaction(ops)
            if r.record.outcome == BUSY:
                stats['busy'] += 1
                return
            self.assertEqual(r.record.outcome, APPLIED)
            stats['applied'] += 1

        forwarded = self.run_with_live_commands(em, [0, 1, 2, 3], churn)
        self.assertGreater(stats['applied'], 10)
        self.assertGreater(sum(forwarded.values()), 0)


suite = unittest.TestLoader().loadTestsFromTestCase(
    BessDataplaneTransactionsTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
