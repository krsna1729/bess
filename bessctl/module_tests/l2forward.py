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

from test_utils import *


class BessL2ForwardTest(BessModuleTestCase):

    def test_l2forward(self):
        l2fib = L2Forward()

        l2fib.add(entries=[{'addr': '00:01:02:03:04:05', 'gate': 64},
                           {'addr': 'aa:bb:cc:dd:ee:ff', 'gate': 1},
                           {'addr': '11:11:11:11:11:22', 'gate': 2}])
        with self.assertRaises(bess.Error):
            l2fib.add(entries=[{'addr': '00:01:02:03:04:05', 'gate': 0}])

        ret = l2fib.lookup(addrs=['aa:bb:cc:dd:ee:ff', '00:01:02:03:04:05'])
        self.assertEqual(ret.gates, [1, 64])

        l2fib.delete(addrs=['00:01:02:03:04:05'])
        with self.assertRaises(bess.Error):
            l2fib.delete(addrs=['00:01:02:03:04:05'])

    # Commands run while workers keep processing (G1.2 mode C) and validate
    # everything before applying anything: int64 wire gates are checked
    # before narrowing, a bad entry adds nothing, and populate with
    # gate_count 0 is refused (it used to divide by zero and crash bessd).
    def test_l2forward_validation(self):
        l2fib = L2Forward()
        for gate in (65536, -1, 8193):
            with self.assertRaises(bess.Error):
                l2fib.add(entries=[{'addr': '00:01:02:03:04:05',
                                    'gate': gate}])
        with self.assertRaises(bess.Error):
            l2fib.add(entries=[{'addr': '00:01:02:03:04:06', 'gate': 1},
                               {'addr': 'not-a-mac', 'gate': 2}])
        with self.assertRaises(bess.Error):
            l2fib.lookup(addrs=['00:01:02:03:04:06'])  # nothing was added
        with self.assertRaises(bess.Error):
            l2fib.add(entries=[{'addr': '00:01:02:03:04:07', 'gate': 1},
                               {'addr': '00:01:02:03:04:07', 'gate': 2}])
        with self.assertRaises(bess.Error):
            l2fib.populate(base='00:01:02:03:00:00', count=10, gate_count=0)
        self.assertBessAlive()
        with self.assertRaises(bess.Error):
            l2fib.set_default_gate(gate=65536)
        l2fib.add(entries=[{'addr': '00:01:02:03:04:08', 'gate': 5}])
        self.assertEqual(list(l2fib.lookup(addrs=['00:01:02:03:04:08']).gates),
                         [5])

suite = unittest.TestLoader().loadTestsFromTestCase(BessL2ForwardTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
