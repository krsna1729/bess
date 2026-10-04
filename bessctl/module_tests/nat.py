# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *

import time


class BessNatTest(BessModuleTestCase):
    # Test the packet mangling features with a single rule

    def _test_l4(self, module, l4_orig, ruleaddr):

        def _swap_l4(l4):
            ret = l4.copy()
            if type(l4) == scapy.UDP or type(l4) == scapy.TCP:
                ret.sport = l4.dport
                ret.dport = l4.sport
            return ret

        # There are 4 packets in this test:
        # 1. orig, 2. natted, 3. reply, 4. unnatted
        #
        # We're acting as 's0' and 's1' to test 'NAT'
        #
        # +----+    pkt_orig   +-------+    pkt_natted   +----+
        # |    |-------------->|       |---------------->|    |
        # | s0 |               |  NAT  |                 | s1 |
        # |    |<--------------|       |<----------------|    |
        # +----+  pkt_unnatted +-------+    pkt_repl     +----+

        eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
        ip_orig = scapy.IP(src='172.16.0.2', dst='8.8.8.8')
        ip_natted = scapy.IP(src=ruleaddr, dst='8.8.8.8')
        ip_reply = scapy.IP(src='8.8.8.8', dst=ruleaddr)
        ip_unnatted = scapy.IP(src='8.8.8.8', dst='172.16.0.2')
        l7 = 'helloworld'

        pkt_orig = eth / ip_orig / l4_orig / l7

        pkt_outs = self.run_module(module, 0, [pkt_orig], [0, 1])
        self.assertEqual(len(pkt_outs[1]), 1)
        pkt_natted = pkt_outs[1][0]

        # The NAT module can choose an arbitrary source port/id.
        # We cannot test it, we have to read from the output.
        l4_natted = l4_orig.copy()
        if type(l4_natted) == scapy.ICMP:
            l4_natted.id = pkt_natted[scapy.ICMP].id
        elif type(l4_natted) == scapy.UDP:
            l4_natted.sport = pkt_natted[scapy.UDP].sport
        elif type(l4_natted) == scapy.TCP:
            l4_natted.sport = pkt_natted[scapy.TCP].sport
        self.assertSamePackets(eth / ip_natted / l4_natted / l7, pkt_natted)

        l4_reply = _swap_l4(l4_natted)
        pkt_reply = eth / ip_reply / l4_reply / l7

        pkt_outs = self.run_module(module, 1, [pkt_reply], [0, 1])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(eth / ip_unnatted / _swap_l4(l4_orig) / l7,
                               pkt_outs[0][0])

    def test_nat_udp(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        nat = NAT(ext_addrs=nat_config)
        self._test_l4(nat, scapy.UDP(sport=56797, dport=53), '192.168.1.1')

    def test_nat_udp_with_zero_cksum(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        nat = NAT(ext_addrs=nat_config)
        self._test_l4(
            nat, scapy.UDP(sport=56797, dport=53, chksum=0), '192.168.1.1')

    def test_nat_tcp(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        nat = NAT(ext_addrs=nat_config)
        self._test_l4(nat, scapy.TCP(sport=52428, dport=80), '192.168.1.1')

    def test_nat_icmp(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        nat = NAT(ext_addrs=nat_config)
        self._test_l4(nat, scapy.ICMP(), '192.168.1.1')

    def test_nat_shared(self):
        # One NAT for every worker (TP6): the same translations.
        nat_config = [{'ext_addr': '192.168.1.1'}]
        for l4 in (scapy.UDP(sport=56797, dport=53),
                   scapy.TCP(sport=52428, dport=80), scapy.ICMP()):
            nat = NAT(ext_addrs=nat_config, shared=True, capacity=1024)
            self._test_l4(nat, l4, '192.168.1.1')

    def test_nat_grows_without_stopping(self):
        # Owned (TP5) and shared (TP6): starts at 4 mappings, may grow to 64;
        # at 3 of 4 it asks the daemon's maintenance loop for a larger table
        # and keeps translating. 40 flows all get a mapping only if the table
        # grew, four times.
        for shared in (False, True):
            self._grow(NAT(ext_addrs=[{'ext_addr': '192.168.1.1'}], capacity=4,
                           max_capacity=64, shared=shared))

    def _grow(self, nat):
        eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
        translated = 0
        for flow in range(40):
            pkt = eth / scapy.IP(src='172.16.0.2', dst='8.8.8.8') / \
                scapy.UDP(sport=20000 + flow, dport=53) / 'x'
            # A packet that finds the table full is dropped: retry while the
            # control side allocates (every 1 ms) and the worker migrates.
            for _ in range(200):
                out = self.run_module(nat, 0, [pkt], [0, 1])
                if len(out[1]) == 1:
                    translated += 1
                    break
                time.sleep(0.01)
        self.assertEqual(40, translated)
        self.assertIn('40 entries', self.bess.get_module_info(nat.name).desc)

    def test_nat_usage(self):
        # Usage counters (TP7): an interim report with the counts so far,
        # owned and shared.
        eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
        for shared in (False, True):
            nat = NAT(ext_addrs=[{'ext_addr': '192.168.1.1'}], capacity=64,
                      usage=True, shared=shared)
            out = scapy.IP(src='172.16.0.2', dst='8.8.8.8') / \
                scapy.UDP(sport=33000, dport=53) / ('y' * 72)  # 100 IP bytes
            for _ in range(3):
                outs = self.run_module(nat, 0, [eth / out], [0, 1])
                self.assertEqual(len(outs[1]), 1)
            nat.request_usage_report()
            # The report walks the table on the packet path: one more batch
            # (a packet that is dropped: an inbound one with no mapping).
            stray = eth / scapy.IP(src='8.8.8.8', dst='192.168.1.1') / \
                scapy.UDP(sport=53, dport=9) / 'z'
            self.run_module(nat, 1, [stray], [0, 1])
            resp = nat.drain_usage()
            self.assertEqual(resp.reports_done, 1)
            self.assertEqual(len(resp.records), 1)
            record = resp.records[0]
            self.assertFalse(record.final)
            self.assertEqual((record.internal_addr, record.internal_port),
                             ('172.16.0.2', 33000))
            self.assertEqual((record.packets, record.bytes), (3, 300))

    def test_nat_selfconfig(self):
        # Send initial conf unsorted, see that it comes back sorted
        # (note that this is a bit different from other modules
        # where argument order often matters).
        iconf = {'ext_addrs': [{'ext_addr': '192.168.1.1',
                                'port_ranges': [{'begin': 1, 'end': 1024},
                                                {'begin': 1025, 'end': 65535}]}]}
        nat = NAT(**iconf)
        arg = pb_conv.protobuf_to_dict(nat.get_initial_arg())
        expect_config = {}
        cur_config = pb_conv.protobuf_to_dict(nat.get_runtime_config())
        print("arg ", arg)
        print("iconf", iconf)
        print("cur_config", cur_config)
        print("expected_config", expect_config)
        assert arg == iconf and cur_config == expect_config


suite = unittest.TestLoader().loadTestsFromTestCase(BessNatTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
