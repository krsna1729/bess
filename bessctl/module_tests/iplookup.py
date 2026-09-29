# Copyright (c) 2017  Tamas Levai <levait@tmit.bme.hu>
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessIPLookupTest(BessModuleTestCase):

    def test_iplookup(self):
        ipl = IPLookup()
        pkts = [get_tcp_packet(sip='12.22.22.22', dip='22.22.22.22'),
                get_tcp_packet(sip='12.22.22.22', dip='32.22.22.22'),
                get_tcp_packet(sip='12.22.22.22', dip='42.22.22.22')]

        ipl.add(prefix='22.22.22.0', prefix_len=24, gate=0)
        ipl.add(prefix='32.22.22.0', prefix_len=24, gate=1)
        ipl.add(prefix='42.22.22.0', prefix_len=24, gate=1)

        ipl.delete(prefix='42.22.22.0', prefix_len=24)
        with self.assertRaises(bess.Error):
            ipl.delete(prefix='52.22.22.0', prefix_len=24)

        pkt_outs = self.run_module(ipl, 0, pkts, [0, 1])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertEqual(len(pkt_outs[1]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkts[0])
        self.assertSamePackets(pkt_outs[1][0], pkts[1])

    def test_prefix(self):
        ipl = IPLookup()
        with self.assertRaises(bess.Error):
            ipl.add(prefix='22.22.22.0', prefix_len=16, gate=0)

    # Routes change while a worker forwards traffic (K7, mode C).
    def test_iplookup_live_commands(self):
        ipl = IPLookup()
        ipl.add(prefix='0.0.0.0', prefix_len=0, gate=0)

        def command(i):
            prefix = '%d.%d.0.0' % (1 + (i // 2) % 200, (i // 400) % 256)
            if i % 2 == 0:
                ipl.add(prefix=prefix, prefix_len=16, gate=1)
            else:
                ipl.delete(prefix=prefix, prefix_len=16)

        pkts = self.run_with_live_commands(ipl, [0, 1], command)
        self.assertGreater(sum(pkts.values()), 0)

    # The gate is uint64 on the wire; 65536 + g used to route to gate g.
    def test_iplookup_gate_does_not_wrap(self):
        ipl = IPLookup()
        with self.assertRaises(bess.Error):
            ipl.add(prefix='10.0.0.0', prefix_len=8, gate=65536 + 1)
        with self.assertRaises(bess.Error):
            ipl.add(prefix='10.0.0.0', prefix_len=8, gate=2 ** 32)
        ipl.add(prefix='10.0.0.0', prefix_len=8, gate=1)


suite = unittest.TestLoader().loadTestsFromTestCase(BessIPLookupTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
