# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

import errno

from test_utils import *


# ACL rules are published atomically (G1.2 mode G): add/clear run while
# workers keep processing, first match wins, unmatched traffic is dropped,
# and a refused command changes nothing. Wire values are validated before
# narrowing (a uint32 port of 65616 is not port 80), and malformed prefixes
# are refused instead of crashing the daemon.
class BessAclTest(BessModuleTestCase):

    def passes(self, acl, pkt):
        outs = self.run_module(acl, 0, [pkt], [0])
        return len(outs[0]) == 1

    def assertRefused(self, fn, **kwargs):
        try:
            fn(**kwargs)
        except BESS.Error as e:
            self.assertEqual(e.code, errno.EINVAL, e.errmsg)
            return
        self.fail('command was accepted')

    def test_rules(self):
        a = get_tcp_packet(sip='10.1.2.3', dip='20.0.0.1', sport=1000, dport=80)
        b = get_tcp_packet(sip='11.1.2.3', dip='20.0.0.1', sport=1000, dport=80)

        acl = ACL(rules=[{'src_ip': '10.0.0.0/8', 'drop': False}])
        self.assertTrue(self.passes(acl, a))
        self.assertFalse(self.passes(acl, b), 'unmatched traffic is dropped')

        # 65616 would narrow to 80 and forward `b`; it must be refused.
        self.assertRefused(acl.add, rules=[{'dst_port': 65616, 'drop': False}])
        self.assertFalse(self.passes(acl, b), 'refused add changes nothing')

        # A malformed prefix used to throw out of Ipv4Prefix (std::stoi).
        self.assertRefused(acl.add, rules=[{'src_ip': '11.0.0.0/x',
                                            'drop': False}])
        self.assertRefused(acl.add, rules=[{'src_ip': '11.0.0.0/8',
                                            'drop': False},
                                           {'dst_ip': 'bogus/8'}])
        self.assertBessAlive()
        self.assertFalse(self.passes(acl, b), 'all-or-nothing')

        acl.add(rules=[{'dst_port': 80, 'drop': False}])
        self.assertTrue(self.passes(acl, b))

        acl.clear()
        self.assertFalse(self.passes(acl, a))
        self.assertFalse(self.passes(acl, b))

    def test_first_match_wins(self):
        a = get_tcp_packet(sip='10.1.2.3', dip='20.0.0.1')
        acl = ACL(rules=[{'src_ip': '10.0.0.0/8', 'drop': True},
                         {'src_ip': '10.1.0.0/16', 'drop': False}])
        self.assertFalse(self.passes(acl, a))

    # Rules are added and cleared while a worker forwards traffic (mode G).
    def test_live_commands(self):
        acl = ACL(rules=[{'src_ip': '0.0.0.0/0', 'drop': False}])

        def command(i):
            if i % 10 == 9:
                acl.clear()
                acl.add(rules=[{'src_ip': '0.0.0.0/0', 'drop': False}])
            else:
                acl.add(rules=[{'dst_port': 1000 + i, 'drop': True}])

        pkts = self.run_with_live_commands(acl, [0], command)
        self.assertGreater(pkts.get(0, 0), 0)

suite = unittest.TestLoader().loadTestsFromTestCase(BessAclTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
