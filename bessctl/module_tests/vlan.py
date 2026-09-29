# Copyright (c) 2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

import sys
from test_utils import *


class BessVlanTest(BessModuleTestCase):

    def test_run_vlan(self):
        self.run_for(VLANSplit(), [0], 3)
        self.assertBessAlive()

    def _vlan_output_test(self, vids, double_tag=False, default_gate=0):
        def gen_vlan_packet(vid):
            eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
            vlan1 = scapy.Dot1AD(vlan=vid)
            vlan2 = scapy.Dot1Q(vlan=vid)
            ip = scapy.IP()
            udp = scapy.UDP(sport=10001, dport=10002)
            payload = 'truculence'
            single_tag = eth / vlan2 / ip / udp / payload
            double_tag = eth / vlan1 / vlan2 / ip / udp / payload
            return single_tag, double_tag

        def gen_untagged_packet():
            eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
            ip = scapy.IP()
            udp = scapy.UDP(sport=10001, dport=10002)
            payload = 'truculence'
            pkt = eth / ip / udp / payload
            return pkt

        expected = []

        vlan = VLANSplit()

        for vid in vids:
            p, pp = gen_vlan_packet(vid)
            q = gen_untagged_packet()
            if vid >= 0:
                if not double_tag:
                    pkt_outs = self.run_module(vlan, 0, [p], [vid])
                    self.assertEqual(len(pkt_outs[vid]), 1)
                    self.assertSamePackets(pkt_outs[vid][0], q)

                else:
                    pkt_outs = self.run_module(vlan, 0, [pp], [vid])
                    self.assertEqual(len(pkt_outs[vid]), 1)
                    self.assertSamePackets(pkt_outs[vid][0], p)

            else:
                pkt_outs = self.run_module(vlan, 0, [q], [default_gate])
                self.assertEqual(len(pkt_outs[default_gate]), 1)
                self.assertSamePackets(pkt_outs[default_gate][0], q)

    @unittest.skipUnless(hasattr(scapy, 'Dot1AD'), "this scapy lacks Dot1AD")
    def test_vlan_single_tag(self):
        self._vlan_output_test([1, 17, -1, 29, 10, 13, 7])

    @unittest.skipUnless(hasattr(scapy, 'Dot1AD'), "this scapy lacks Dot1AD")
    def test_vlan_double_tag(self):
        self._vlan_output_test([1, 17, -1, 29, 10, 13, 7], True)

suite = unittest.TestLoader().loadTestsFromTestCase(BessVlanTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
