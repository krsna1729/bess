# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *

# VXLANDecap checks the outer headers through the tunnel library (M19, D-069):
# a valid frame on any UDP port is decapsulated, with or without a VLAN tag on
# the outer Ethernet header; anything that does not check is dropped.


class BessVxlanTest(BessModuleTestCase):

    def inner(self):
        return scapy.Ether(src='02:00:00:00:00:01', dst='02:00:00:00:00:02') / \
            scapy.IP(src='10.0.0.1', dst='10.0.0.2') / \
            scapy.UDP(sport=1000, dport=2000) / 'helloworld'

    def outer(self, payload, vlan=False, dport=4789, flags=0x08):
        eth = scapy.Ether(src='02:00:00:00:00:aa', dst='02:00:00:00:00:bb')
        if vlan:
            eth = eth / scapy.Dot1Q(vlan=7)
        return eth / scapy.IP(src='192.0.2.1', dst='192.0.2.2') / \
            scapy.UDP(sport=49152, dport=dport) / \
            scapy.VXLAN(flags=flags, vni=0x1234) / payload

    def test_decap_valid(self):
        for vlan in (False, True):
            for dport in (4789, 8472):
                vd = VXLANDecap()
                outs = self.run_module(vd, 0, [self.outer(self.inner(), vlan, dport)], [0])
                self.assertEqual(len(outs[0]), 1, (vlan, dport))
                self.assertSamePackets(outs[0][0], self.inner())

    def test_decap_drops_what_does_not_check(self):
        bad = [
            self.outer(self.inner(), flags=0x00),  # I flag clear
            scapy.Ether() / scapy.IPv6() / scapy.UDP(dport=4789) /
            scapy.VXLAN(vni=1) / self.inner(),     # IPv6 outer (metadata is IPv4)
            scapy.Ether() / scapy.IP() / scapy.TCP(),  # not UDP
        ]
        truncated = bytes(self.outer(self.inner()))[:40]
        bad.append(scapy.Ether(truncated))
        vd = VXLANDecap()
        outs = self.run_module(vd, 0, bad, [0])
        self.assertEqual(len(outs[0]), 0)

    def test_encap_runs(self):
        ve = VXLANEncap(dstport=4789)
        self.run_for(ve, [0], 3)
        self.assertBessAlive()


suite = unittest.TestLoader().loadTestsFromTestCase(BessVxlanTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
