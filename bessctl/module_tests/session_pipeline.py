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
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
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

# The session vertical slice against a live daemon (G1.2b): ExactMatch (action
# mode) -> ActionTable -> Meter -> Router, created as one transactional
# resource graph and steered with one ApplyTransaction, with packets flowing
# through the real module graph on the way.

import socket
import struct
from test_utils import *
from builtin_pb import control_v2_pb2 as control_v2
from builtin_pb import module_msg_pb2 as module_msg

APPLIED = control_v2.TransactionRecord.OUTCOME_APPLIED
REJECTED = control_v2.TransactionRecord.OUTCOME_REJECTED

# Where a session's next hop sends the packet: a router output gate, wired to
# the test's output port.
EGRESS_GATE = 7

# The rule's fields: 4 bytes at offset 26 (IPv4 source), 2 bytes at offset 34
# (the TCP/UDP source port).
FIELDS = [{'offset': 26, 'num_bytes': 4}, {'offset': 34, 'num_bytes': 2}]


def meter_op(bess, module, meter_id, committed_burst):
    """A meter whose buckets hold `committed_burst` bytes and never refill."""
    return bess.transaction_op(
        module.name + '/meters', module_msg.MeterIdKey(id=meter_id),
        module_msg.MeterPolicyValue(tr_tcm=module_msg.TrTcmProfile(
            committed_rate=1, committed_burst=committed_burst, peak_rate=1,
            peak_burst=committed_burst)))


def next_hop_op(bess, module, hop_id, gate):
    return bess.transaction_op(
        module.name + '/next_hops', module_msg.RouterNextHopIdKey(id=hop_id),
        module_msg.RouterNextHopValue(egress_gate=gate))


def route_op(bess, module, ipv4, prefix_length, hop_id):
    return bess.transaction_op(
        module.name + '/routes',
        module_msg.RouterRouteKey(ipv4=ipv4, prefix_length=prefix_length),
        module_msg.RouterRouteValue(next_hop_id=hop_id))


def action_op(bess, module, action_id, meter_id, hop_id):
    return bess.transaction_op(
        module.name + '/actions', module_msg.ActionIdKey(id=action_id),
        module_msg.ActionValue(meter_id=meter_id, next_hop_id=hop_id))


def rule_key(sip, sport):
    key = module_msg.ExactMatchRuleKey()
    key.fields.add(value_bin=socket.inet_aton(sip))
    key.fields.add(value_bin=struct.pack('!H', sport))
    return key


def rule_op(bess, module, sip, sport, action_id):
    return bess.transaction_op(module.name + '/rules', rule_key(sip, sport),
                               module_msg.ExactMatchRuleValue(
                                   action_id=action_id))


def erase_op(bess, module, resource, key):
    return bess.transaction_op(module.name + '/' + resource, key, erase=True)


class BessSessionPipelineTest(BessModuleTestCase):

    def build_pipeline(self, connect=True):
        """The four modules, with the session's resources named.

        The ActionTable and the ExactMatch are created first and name resources
        of modules that do not exist yet: a declared reference binds when its
        resource registers, so module creation order does not decide the graph.
        """
        at = ActionTable(name='at', capacity=64, meters='mt/meters',
                         next_hops='rt/next_hops')
        em = ExactMatch(fields=FIELDS, action_resource='at/actions')
        mt = Meter(name='mt', capacity=64)
        rt = Router(name='rt', max_routes=64, max_tbl8s=16, max_next_hops=64)
        if connect:
            em: 0 -> at
            at: 0 -> mt
            # Only green reaches the router: yellow and red are left
            # unconnected, so those packets are dropped -- the policy is the
            # graph.
            mt: 0 -> rt
        return em, at, mt, rt

    def session_ops(self, em, at, mt, rt, sip, sport, action_id=1,
                    meter_id=1, hop_id=1, burst=100, hop_gate=EGRESS_GATE):
        return [
            meter_op(self.bess, mt, meter_id, burst),
            next_hop_op(self.bess, rt, hop_id, hop_gate),
            route_op(self.bess, rt, '10.0.0.0', 8, hop_id),
            action_op(self.bess, at, action_id, meter_id, hop_id),
            rule_op(self.bess, em, sip, sport, action_id),
        ]

    def test_session_transaction_steers_and_polices(self):
        em, at, mt, rt = self.build_pipeline()
        names = [res.name for res in
                 self.bess.list_transaction_resources().resources]
        for name in (em.name + '/rules', at.name + '/actions',
                     mt.name + '/meters', rt.name + '/next_hops',
                     rt.name + '/routes'):
            self.assertIn(name, names)

        # One transaction creates the whole session: the meter, the next hop,
        # the route to it, the action naming both, and the rule that selects
        # the action.
        sip, sport = '10.1.2.3', 1234
        r = self.bess.apply_transaction(
            self.session_ops(em, at, mt, rt, sip, sport))
        self.assertEqual(r.record.outcome, APPLIED,
                         r.record.ops[0].error if r.record.ops else '')

        # Two 60-byte packets of the session: the meter's 100-byte buckets take
        # the first (green -> the router -> the next hop's egress) and not the
        # second (red -> dropped).
        pkt1 = get_tcp_packet(sip=sip, sport=sport, dip='10.9.9.9', dport=80)
        pkt2 = get_tcp_packet(sip=sip, sport=sport, dip='10.9.9.9', dport=80)
        outs = self.run_pipeline(em, rt, 0, [pkt1, pkt2], [EGRESS_GATE])
        self.assertEqual(len(outs[EGRESS_GATE]), 1)
        self.assertSamePackets(outs[EGRESS_GATE][0], pkt1)

    def test_other_sessions_and_misses_are_dropped(self):
        em, at, mt, rt = self.build_pipeline()
        r = self.bess.apply_transaction(
            self.session_ops(em, at, mt, rt, '10.1.2.3', 1234))
        self.assertEqual(r.record.outcome, APPLIED)

        # A session that matches but names no next hop: the action resolves and
        # the router has nothing to resolve.
        r = self.bess.apply_transaction([
            action_op(self.bess, at, 2, 0, 0),
            rule_op(self.bess, em, '10.4.4.4', 4321, 2),
        ])
        self.assertEqual(r.record.outcome, APPLIED,
                         r.record.ops[0].error if r.record.ops else '')

        # A packet whose rule does not match takes the ExactMatch default gate
        # (drop): no session, no forwarding.
        miss = get_tcp_packet(sip='192.0.2.1', sport=1234, dip='10.9.9.9',
                              dport=80)
        # A packet whose action names no next hop: dropped at the router.
        other = get_tcp_packet(sip='10.4.4.4', sport=4321, dip='10.9.9.9',
                               dport=80)
        outs = self.run_pipeline(em, rt, 0, [miss, other], [EGRESS_GATE])
        self.assertEqual(len(outs[EGRESS_GATE]), 0)

    def test_removing_what_a_session_names_is_refused(self):
        em, at, mt, rt = self.build_pipeline()
        r = self.bess.apply_transaction(
            self.session_ops(em, at, mt, rt, '10.1.2.3', 1234))
        self.assertEqual(r.record.outcome, APPLIED)

        # The action names the meter: it may not go while it is still named,
        # and the rejected transaction leaves everything in place.
        r = self.bess.apply_transaction(
            [erase_op(self.bess, mt, 'meters', module_msg.MeterIdKey(id=1))])
        self.assertEqual(r.record.outcome, REJECTED)
        self.assertIn('still referenced', r.record.ops[0].error)

        # Removing the session's names in the same transaction as the action
        # works: the rule, the action, the route, the next hop and the meter go
        # together.
        r = self.bess.apply_transaction([
            erase_op(self.bess, em, 'rules', rule_key('10.1.2.3', 1234)),
            erase_op(self.bess, at, 'actions', module_msg.ActionIdKey(id=1)),
            erase_op(self.bess, rt, 'routes',
                     module_msg.RouterRouteKey(ipv4='10.0.0.0',
                                               prefix_length=8)),
            erase_op(self.bess, rt, 'next_hops',
                     module_msg.RouterNextHopIdKey(id=1)),
            erase_op(self.bess, mt, 'meters', module_msg.MeterIdKey(id=1)),
        ])
        self.assertEqual(r.record.outcome, APPLIED,
                         r.record.ops[0].error if r.record.ops else '')

        # The session is gone: its packets are dropped.
        pkt = get_tcp_packet(sip='10.1.2.3', sport=1234, dip='10.9.9.9',
                             dport=80)
        outs = self.run_pipeline(em, rt, 0, [pkt], [EGRESS_GATE])
        self.assertEqual(len(outs[EGRESS_GATE]), 0)

    def test_transactions_while_traffic_runs(self):
        # The ExactMatch in action mode is written by transactions while a
        # worker forwards traffic through it: rules and the actions they name
        # come and go under packets.
        em, at, mt, rt = self.build_pipeline(connect=False)
        em.set_default_gate(gate=0)
        stats = {'applied': 0}

        def churn(i):
            ops = [action_op(self.bess, at, 1 + i % 4, 0, 0),
                   rule_op(self.bess, em, '10.%d.%d.1' % (i // 256 % 256,
                                                          i % 256), 1,
                           1 + i % 4)]
            if i >= 4:
                ops.append(erase_op(
                    self.bess, em, 'rules',
                    rule_key('10.%d.%d.1' % ((i - 4) // 256 % 256,
                                             (i - 4) % 256), 1)))
            r = self.bess.apply_transaction(ops)
            self.assertEqual(r.record.outcome, APPLIED,
                             r.record.ops[0].error if r.record.ops else '')
            stats['applied'] += 1

        forwarded = self.run_with_live_commands(em, [0], churn)
        self.assertGreater(stats['applied'], 10)
        self.assertGreater(sum(forwarded.values()), 0)


suite = unittest.TestLoader().loadTestsFromTestCase(BessSessionPipelineTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
