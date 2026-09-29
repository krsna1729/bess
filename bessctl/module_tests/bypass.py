# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessBypassTest(BessModuleTestCase):

    def test_bypass(self):
        bp0 = Bypass()
        pkt_in = get_udp_packet()
        pkt_outs = self.run_module(bp0, 0, [pkt_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt_in)

        pkt_outs = self.run_module(bp0, 0, [], [0])
        self.assertEqual(len(pkt_outs[0]), 0)

suite = unittest.TestLoader().loadTestsFromTestCase(BessBypassTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
