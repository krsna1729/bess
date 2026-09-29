# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessRandomSplitTest(BessModuleTestCase):

    def test_dropnone(self):
        drop0 = RandomSplit(drop_rate=0, gates=[0])
        pkt_in = get_udp_packet()
        pkt_outs = self.run_module(drop0, 0, [pkt_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt_in)

    def test_dropall(self):
        drop0 = RandomSplit(drop_rate=1, gates=[0])
        pkt_in = get_udp_packet()
        pkt_outs = self.run_module(drop0, 0, [pkt_in], [0])
        self.assertEqual(len(pkt_outs[0]), 0)

    def _drop_with_rate(self, rate):

        def _equal_with_noise(a, b, threshold):
            return abs((a - b)) <= threshold

        pktftm = [
            bytes(get_udp_packet()),
            bytes(get_tcp_packet())]

        ma = Measure()
        mb = Measure()

        Source() -> \
            ma -> \
            Rewrite(templates=pktftm) -> \
            RandomSplit(drop_rate=rate, gates=[0]) -> \
            mb -> \
            Sink()

        bess.resume_all()
        time.sleep(1)
        bess.pause_all()

        # Measure the ratio of packets dropped
        ratio = float(mb.get_summary().packets) / ma.get_summary().packets
        assert _equal_with_noise(ratio, 1 - rate, 0.05)

    def test_droprate_1(self):
        self._drop_with_rate(0.3)

    def test_droprate_2(self):
        self._drop_with_rate(0.5)

    def test_droprate_3(self):
        self._drop_with_rate(0.75)

    def test_droprate_4(self):
        self._drop_with_rate(0.9)

suite = unittest.TestLoader().loadTestsFromTestCase(BessRandomSplitTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
