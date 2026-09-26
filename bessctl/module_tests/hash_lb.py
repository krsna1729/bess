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


# HashLB's configuration is published atomically (G1.2 mode G): commands run
# while workers keep processing, and a refused command changes nothing. In
# particular a wire gate that does not fit gate_idx_t (65536) must be refused,
# not narrowed to gate 0.
class BessHashLBTest(BessModuleTestCase):

    def flows(self, n=64):
        return [get_tcp_packet(sip='10.0.%d.%d' % (i // 250, i % 250 + 1),
                               dip='10.1.0.1', sport=1000 + i, dport=80)
                for i in range(n)]

    def gates_used(self, module, pkts, candidates):
        outs = self.run_module(module, 0, pkts, candidates)
        return sorted(g for g in candidates if len(outs[g]) > 0), \
            sum(len(outs[g]) for g in candidates)

    def assertRefused(self, fn, *args, **kwargs):
        try:
            fn(*args, **kwargs)
        except BESS.Error as e:
            self.assertEqual(e.code, errno.EINVAL, e.errmsg)
            return
        self.fail('command was accepted')

    def test_no_gates_is_refused_at_creation(self):
        with self.assertRaises(BESS.Error) as ctx:
            HashLB()
        self.assertEqual(ctx.exception.code, errno.EINVAL)

    def test_gates_and_modes(self):
        pkts = self.flows()
        lb = HashLB(gates=[1, 2])
        used, total = self.gates_used(lb, pkts, range(8))
        self.assertEqual(total, len(pkts))
        self.assertEqual(used, [1, 2], 'all flows on the configured gates')

        lb.set_gates(gates=[3, 4, 5])
        used, total = self.gates_used(lb, pkts, range(8))
        self.assertEqual(total, len(pkts))
        self.assertEqual(used, [3, 4, 5])

        # 65536 would narrow to gate 0; it must be refused, and a bad gate
        # later in the list must not leave a half-written list behind.
        self.assertRefused(lb.set_gates, gates=[65536])
        self.assertRefused(lb.set_gates, gates=[6, 7, 70000])
        # No destinations: every packet would index an empty gate list.
        self.assertRefused(lb.set_gates, gates=[])
        used, total = self.gates_used(lb, pkts, range(8))
        self.assertEqual(total, len(pkts))
        self.assertEqual(used, [3, 4, 5], 'refused commands change nothing')

        lb.set_mode(mode='l3')
        used, total = self.gates_used(lb, pkts, range(8))
        self.assertEqual(total, len(pkts))
        self.assertTrue(set(used) <= {3, 4, 5})
        self.assertRefused(lb.set_mode, mode='bogus')
        used, total = self.gates_used(lb, pkts, range(8))
        self.assertEqual(total, len(pkts))


suite = unittest.TestLoader().loadTestsFromTestCase(BessHashLBTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
