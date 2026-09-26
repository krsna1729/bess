# Copyright (c) 2016-2017, Nefeli Networks, Inc.
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


suite = unittest.TestLoader().loadTestsFromTestCase(BessAclTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
