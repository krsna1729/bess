# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessUpdateTTLTest(BessModuleTestCase):

    def test_run_update_ttl(self):
        uttl = UpdateTTL()
        self.run_for(uttl, [0], 3)

    def test_decrement(self):
        uttl = UpdateTTL()

        pkt_in = get_tcp_packet()
        pkt_in[scapy.IP].ttl = 2
        pkt_expected_out = scapy.Packet.copy(pkt_in)
        pkt_expected_out[scapy.IP].ttl = 1

        pkt_outs = self.run_module(uttl, 0, [pkt_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt_expected_out)

    def test_drop(self):
        # Drop test
        uttl = UpdateTTL()

        pkt_in = get_tcp_packet()
        pkt_in[scapy.IP].ttl = 2
        pkt_expected_out = scapy.Packet.copy(pkt_in)
        pkt_expected_out[scapy.IP].ttl = 1

        drop_pkt0 = get_tcp_packet()
        drop_pkt0[scapy.IP].ttl = 0
        drop_pkt1 = get_tcp_packet()
        drop_pkt1[scapy.IP].ttl = 1

        pkt_outs = self.run_module(uttl, 0, [drop_pkt0], [0])
        self.assertEqual(len(pkt_outs[0]), 0)

        pkt_outs = self.run_module(uttl, 0, [pkt_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt_expected_out)

        pkt_outs = self.run_module(uttl, 0, [drop_pkt1], [0])
        self.assertEqual(len(pkt_outs[0]), 0)

suite = unittest.TestLoader().loadTestsFromTestCase(BessUpdateTTLTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
