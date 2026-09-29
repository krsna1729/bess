# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessIpChecksumTest(BessModuleTestCase):

    def test_bypass(self):
        wmodule = IPChecksum()

        eth = scapy.Ether(src='de:ad:be:ef:12:34', dst='12:34:de:ad:be:ef')
        vlan = scapy.Dot1Q(vlan=6)
        ip_wrong = scapy.IP(
            src="1.2.3.4", dst="2.3.4.5", ttl=98, chksum=0x0000)
        ip_right = scapy.IP(src="1.2.3.4", dst="2.3.4.5", ttl=98)
        udp = scapy.UDP(sport=10001, dport=10002)
        payload = 'helloworldhelloworldhelloworld'

        in_out = []

        eth_in = eth / ip_wrong / udp / payload
        eth_out = eth / ip_right / udp / payload
        self.assertNotSamePackets(eth_in, eth_out)

        vlan_in = eth / vlan / ip_wrong / udp / payload
        vlan_out = eth / vlan / ip_right / udp / payload
        self.assertNotSamePackets(vlan_in, vlan_out)

        pkt_outs = self.run_module(wmodule, 0, [eth_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], eth_out)

        pkt_outs = self.run_module(wmodule, 0, [vlan_in], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], vlan_out)

        # scapy-python3 doesn't have Dot1AD
        if hasattr(scapy, 'Dot1AD'):
            qinq = scapy.Dot1AD(vlan=5)

            qinq_in = eth / qinq / vlan / ip_wrong / udp / payload
            qinq_out = eth / qinq / vlan / ip_right / udp / payload
            self.assertNotSamePackets(qinq_in, qinq_out)

            pkt_outs = self.run_module(wmodule, 0, [qinq_in], [0])
            self.assertEqual(len(pkt_outs[0]), 1)
            self.assertSamePackets(pkt_outs[0][0], qinq_out)

suite = unittest.TestLoader().loadTestsFromTestCase(BessIpChecksumTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
