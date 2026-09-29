# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessDecapTest(BessModuleTestCase):

    def test_run_decap_0bytes(self):
        gd = GenericDecap(bytes=0)
        self.run_for(gd, [0], 3)
        self.assertBessAlive()

    def test_run_decap_morebytes(self):
        gd = GenericDecap(bytes=23)
        self.run_for(gd, [0], 3)
        self.assertBessAlive()

    # test strip off ether
    def test_decap(self):
        gd = GenericDecap(bytes=14)

        eth = scapy.Ether(src='de:ad:be:ef:12:34', dst='12:34:de:ad:be:ef')
        ip = scapy.IP(src="1.2.3.4", dst="2.3.4.5", ttl=98)
        udp = scapy.UDP(sport=10001, dport=10002)
        payload = 'helloworldhelloworldhelloworld'

        pkt_in = eth / ip / udp / payload
        pkt_expected_out = ip / udp / payload

        pkt_outs = self.run_module(gd, 0, [pkt_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt_expected_out)

suite = unittest.TestLoader().loadTestsFromTestCase(BessDecapTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
