# Copyright (c) 2017, Cloudigo.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessArpTest(BessModuleTestCase):

    def test_arp(self):
        arp = ArpResponder()

        eth_header = scapy.Ether(
            src='02:1e:67:9f:4d:ae', dst='ff:ff:ff:ff:ff:ff')
        arp_header = scapy.ARP(op=1, pdst='1.2.3.4')
        arp_req = eth_header / arp_header

        arp.add(ip='1.2.3.4', mac_addr='A0:22:33:44:55:66')

        arp_reply = arp_req.copy()
        arp_reply[scapy.Ether].src = 'A0:22:33:44:55:66'
        arp_reply[scapy.Ether].dst = '02:1e:67:9f:4d:ae'
        arp_reply[scapy.ARP].op = 2

        arp_reply[scapy.ARP].hwdst = arp_req[scapy.ARP].hwsrc
        arp_reply[scapy.ARP].hwsrc = 'A0:22:33:44:55:66'

        arp_reply[scapy.ARP].pdst = arp_req[scapy.ARP].psrc
        arp_reply[scapy.ARP].psrc = '1.2.3.4'

        pkt_outs = self.run_module(arp, 0, [arp_req], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], arp_reply)

suite = unittest.TestLoader().loadTestsFromTestCase(BessArpTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
