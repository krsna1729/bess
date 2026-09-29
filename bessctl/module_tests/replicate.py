# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessReplicateTest(BessModuleTestCase):

    def test_run_replicate4(self):
        rep4 = Replicate(gates=[0, 1, 2, 3])
        self.run_for(rep4, [0], 3)

    def test_run_replicate10(self):
        rep10 = Replicate(gates=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        self.run_for(rep10, [0], 3)

    def test_run_replicate1(self):
        rep1 = Replicate(gates=[0])
        self.run_for(rep1, [0], 3)

    def test_replicate(self):
        rep3 = Replicate(gates=[0, 1, 2])
        pkt_in = get_tcp_packet(sip='22.22.22.22', dip='22.22.22.22')

        pkt_outs = self.run_module(rep3, 0, [pkt_in], [0, 1, 2])

        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt_in)

        self.assertEqual(len(pkt_outs[1]), 1)
        self.assertSamePackets(pkt_outs[1][0], pkt_in)

        self.assertEqual(len(pkt_outs[2]), 1)
        self.assertSamePackets(pkt_outs[2][0], pkt_in)

    # Gates are int64 on the wire and were narrowed unchecked: each must name
    # one of the module's 32 output gates, and a refused set_gates changes
    # nothing.
    def test_replicate_gates_are_validated(self):
        for bad in [[32], [-1], [65536], [0, 1, 2 ** 32]]:
            with self.assertRaises(bess.Error, msg=str(bad)):
                Replicate(gates=bad)
        rep = Replicate(gates=[0, 1])
        bess.pause_all()
        with self.assertRaises(bess.Error):
            rep.set_gates(gates=[2, 40])
        self.assertBessAlive()


suite = unittest.TestLoader().loadTestsFromTestCase(BessReplicateTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
