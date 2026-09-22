# Copyright (c) 2017, Nefeli Networks, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# * Neither the names of the copyright holders nor the names of their
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

import socket
import sys
from test_utils import *
from pybess import protobuf_to_dict as pb_conv


def vstring(*values):
    return [{'value_bin': bytes(bytearray(i))} for i in values]


class BessWildcardMatchTest(BessModuleTestCase):

    def test_run_wildcardmatch(self):
        # basically the same mask/match values as for ExactMatch
        # (not the most thorough test)
        wm = WildcardMatch(fields=[{'offset': 23, 'num_bytes': 1},
                                   {'offset': 2, 'num_bytes': 2},
                                   {'offset': 29, 'num_bytes': 1}])
        mask_list = vstring([0xff], [0xff, 0xff], [0xff])
        wm.add(gate=0, priority=0, masks=mask_list,
               values=vstring([0xff], [0x23, 0xba], [0x34]))
        wm.add(gate=1, priority=0, masks=mask_list,
               values=vstring([0xff], [0x34, 0xaa], [0x12]))
        wm.add(gate=2, priority=0, masks=mask_list,
               values=vstring([0xba], [0x33, 0xaa], [0x22]))
        wm.add(gate=3, priority=0, masks=mask_list,
               values=vstring([0xaa], [0x34, 0xba], [0x32]))
        wm.add(gate=4, priority=0, masks=mask_list,
               values=vstring([0x34], [0x34, 0x7a], [0x52]))
        wm.add(gate=5, priority=0, masks=mask_list,
               values=vstring([0x12], [0x34, 0x7a], [0x72]))
        self.run_for(wm, [0], 3)
        self.assertBessAlive()

    # Output test over fields -- just make sure packets go out right ports
    def test_wildcardmatch(self):
        # Wildcard match for ip src and dst.
        # Make sure higher priority match overrides lower, too.
        wm = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                   {'offset': 30, 'num_bytes': 4}])
        ip_ip = vstring([0xff, 0xff, 0xff, 0xff], [0xff, 0xff, 0xff, 0xff])
        sip1 = '65.43.21.0'
        sip2 = '0.12.34.56'
        dip = '12.34.56.78'
        s1d_pair = [{'value_bin': socket.inet_aton(sip1)},
                    {'value_bin': socket.inet_aton(dip)}]
        wm.add(gate=0, priority=0, masks=ip_ip, values=s1d_pair)
        wm.add(gate=1, priority=1, masks=ip_ip, values=s1d_pair)
        s2d_pair = [{'value_bin': socket.inet_aton(sip2)},
                    {'value_bin': socket.inet_aton(dip)}]
        wm.add(gate=0, priority=0, masks=ip_ip, values=s2d_pair)
        wm.add(gate=2, priority=1, masks=ip_ip, values=s2d_pair)
        wm.set_default_gate(gate=3)

        pkt1 = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')
        pkt2 = get_tcp_packet(sip='0.12.34.56', dip='12.34.56.78')
        pkt_nomatch = get_tcp_packet(sip='0.12.33.56', dip='12.34.56.78')

        pkt_outs = self.run_module(wm, 0, [], range(4))
        for i in range(4):
            self.assertEqual(len(pkt_outs[i]), 0)

        pkt_outs = self.run_module(wm, 0, [pkt1], range(4))
        self.assertEqual(len(pkt_outs[1]), 1)
        self.assertSamePackets(pkt_outs[1][0], pkt1)

        pkt_outs = self.run_module(wm, 0, [pkt2], range(4))
        self.assertEqual(len(pkt_outs[2]), 1)
        self.assertSamePackets(pkt_outs[2][0], pkt2)

        pkt_outs = self.run_module(wm, 0, [pkt_nomatch], range(4))
        self.assertEqual(len(pkt_outs[3]), 1)
        self.assertSamePackets(pkt_outs[3][0], pkt_nomatch)

    def test_wildcardmatch_nonprefix_masks(self):
        # Masks that are not prefixes: arbitrary bit patterns inside each byte.
        wm = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 4}])
        mask = [{'value_bin': bytes(bytearray([0x0f, 0xf0, 0x00, 0xff]))}]
        value = [{'value_bin': bytes(bytearray([0x0a, 0x30, 0x00, 0x44]))}]
        wm.add(gate=1, priority=1, masks=mask, values=value)
        wm.set_default_gate(gate=2)

        # Matches on the masked bits; the wildcard bits differ.
        hit = get_tcp_packet(sip='26.51.119.68', dip='10.0.0.1')
        miss = get_tcp_packet(sip='26.51.119.69', dip='10.0.0.1')

        pkt_outs = self.run_module(wm, 0, [hit], range(3))
        self.assertEqual(len(pkt_outs[1]), 1)
        pkt_outs = self.run_module(wm, 0, [miss], range(3))
        self.assertEqual(len(pkt_outs[2]), 1)

    def test_wildcardmatch_mixed_field_sizes(self):
        # 1-, 3-, and 7-byte fields, so the dense key is not a multiple of
        # eight and no field is word-sized.
        wm = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 1},
                                   {'offset': 27, 'num_bytes': 3},
                                   {'offset': 30, 'num_bytes': 7}])
        masks = vstring([0xff], [0xff, 0xff, 0xff],
                        [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff])
        pkt = get_tcp_packet(sip='26.51.119.68', dip='10.0.0.1')
        raw = bytes(pkt)
        values = vstring([raw[26]], list(raw[27:30]), list(raw[30:37]))
        wm.add(gate=1, priority=1, masks=masks, values=values)
        wm.set_default_gate(gate=2)

        pkt_outs = self.run_module(wm, 0, [pkt], range(3))
        self.assertEqual(len(pkt_outs[1]), 1)

        other = get_tcp_packet(sip='26.51.119.68', dip='10.0.0.9')
        pkt_outs = self.run_module(wm, 0, [other], range(3))
        self.assertEqual(len(pkt_outs[2]), 1)

    def test_wildcardmatch_multiple_masks_priority_and_tie(self):
        # Two different masks both match the same packet.
        wm = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 4}])
        wild = [{'value_bin': bytes(bytearray([0x1a, 0x00, 0x00, 0x00]))}]
        wild_mask = [{'value_bin': bytes(bytearray([0xff, 0x00, 0x00, 0x00]))}]
        exact = [{'value_bin': bytes(bytearray([0x1a, 0x33, 0x77, 0x44]))}]
        exact_mask = [{'value_bin': bytes(bytearray([0xff, 0xff, 0xff, 0xff]))}]
        pkt = get_tcp_packet(sip='26.51.119.68', dip='10.0.0.1')

        # Higher priority wins regardless of which mask was added first.
        wm.add(gate=1, priority=1, masks=wild_mask, values=wild)
        wm.add(gate=2, priority=9, masks=exact_mask, values=exact)
        wm.set_default_gate(gate=3)
        pkt_outs = self.run_module(wm, 0, [pkt], range(4))
        self.assertEqual(len(pkt_outs[2]), 1)

        # Equal priority: the later command wins.
        wm2 = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 4}])
        wm2.add(gate=1, priority=5, masks=wild_mask, values=wild)
        wm2.add(gate=2, priority=5, masks=exact_mask, values=exact)
        wm2.set_default_gate(gate=3)
        pkt_outs = self.run_module(wm2, 0, [pkt], range(4))
        self.assertEqual(len(pkt_outs[2]), 1)

        # ... and reversing the command order reverses the winner.
        wm3 = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 4}])
        wm3.add(gate=2, priority=5, masks=exact_mask, values=exact)
        wm3.add(gate=1, priority=5, masks=wild_mask, values=wild)
        wm3.set_default_gate(gate=3)
        pkt_outs = self.run_module(wm3, 0, [pkt], range(4))
        self.assertEqual(len(pkt_outs[1]), 1)

    def test_wildcardmatch_delete_and_clear(self):
        wm = WildcardMatch(fields=[{'offset': 26, 'num_bytes': 4}])
        mask = [{'value_bin': socket.inet_aton('255.255.255.255')}]
        value = [{'value_bin': socket.inet_aton('26.51.119.68')}]
        wm.add(gate=1, priority=1, masks=mask, values=value)
        wm.set_default_gate(gate=2)
        pkt = get_tcp_packet(sip='26.51.119.68', dip='10.0.0.1')

        pkt_outs = self.run_module(wm, 0, [pkt], range(3))
        self.assertEqual(len(pkt_outs[1]), 1)

        wm.delete(masks=mask, values=value)
        pkt_outs = self.run_module(wm, 0, [pkt], range(3))
        self.assertEqual(len(pkt_outs[2]), 1)

        # clear() must free tuple capacity, not just empty the tables: eight
        # brand-new masks have to fit afterwards. The eighth re-added mask
        # matches the probe packet, so reaching gate 1 proves the new masks are
        # live (and the adds above would have failed on the wire if the old
        # masks still occupied the tuple ceiling).
        for i in range(8):
            wm.add(gate=1, priority=1,
                   masks=[{'value_bin': bytes(bytearray([0xff, 0, 0, i]))}],
                   values=[{'value_bin': bytes(bytearray([0x1a, 0, 0, i]))}])
        wm.clear()
        for i in range(8):
            wm.add(gate=1, priority=1,
                   masks=[{'value_bin': bytes(bytearray([0, 0xff, i, 0]))}],
                   values=[{'value_bin': bytes(bytearray([0, 0x33, i, 0]))}])
        pkt_outs = self.run_module(wm, 0, [pkt], range(3))
        self.assertEqual(len(pkt_outs[1]), 1)

    def test_wildcardmatch_short_packet_takes_default_gate(self):
        # A field reaching past the packet must not be read: the packet goes to
        # the default gate.
        wm = WildcardMatch(fields=[{'offset': 30, 'num_bytes': 4}])
        mask = [{'value_bin': socket.inet_aton('255.255.255.255')}]
        value = [{'value_bin': socket.inet_aton('0.0.0.0')}]
        wm.add(gate=1, priority=1, masks=mask, values=value)
        wm.set_default_gate(gate=2)

        short = scapy.Raw(b'\x00' * 20)
        pkt_outs = self.run_module(wm, 0, [short], range(3))
        self.assertEqual(len(pkt_outs[2]), 1)

    def test_wildcardmatch_with_metadata(self):
        # One wildcard match field
        mask = vstring([0xff, 0xff])
        wm = WildcardMatch(fields=[{"attr_name": "sangjin", "num_bytes": 2}])
        wm.add(gate=1, priority=0, masks=mask, values=vstring([0x88, 0x80]))
        wm.add(gate=2, priority=0, masks=mask, values=vstring([0x77, 0x70]))
        wm.set_default_gate(gate=0)

        # Only need one test packet
        eth = scapy.Ether(src='de:ad:be:ef:12:34', dst='12:34:de:ad:be:ef')
        ip = scapy.IP(src="1.2.3.4", dst="2.3.4.5", ttl=98)
        udp = scapy.UDP(sport=10001, dport=10002)
        payload = 'helloworld'
        test_packet_in = ip / udp / payload

        # Three kinds of metadata tags
        metadata = []
        metadata.append(SetMetadata(
            attrs=[{"name": "sangjin", "size": 2, "value_bin": b'\x77\x90'}]))
        metadata.append(SetMetadata(
            attrs=[{"name": "sangjin", "size": 2, "value_bin": b'\x88\x80'}]))
        metadata.append(SetMetadata(
            attrs=[{"name": "sangjin", "size": 2, "value_bin": b'\x77\x70'}]))

        # And a merge module
        merger = Merge()
        merger -> wm

        for i in range(3):
            metadata[i] -> merger

        for i in range(3):
            pkt_outs = self.run_pipeline(
                metadata[i], wm, 0, [test_packet_in], range(3))
            self.assertEqual(len(pkt_outs[i]), 1)
            self.assertSamePackets(pkt_outs[i][0], test_packet_in)

    def test_wildcardmatch_selfconfig(self):
        "make sure get_initial_arg and [gs]et_runtime_config work"
        iconf = {
            'fields': [{'attr_name': 'babylon5', 'num_bytes': 2},
                       {'offset': 10, 'num_bytes': 1}]
        }
        wm = WildcardMatch(**iconf)
        # workers are all paused, we never run them here
        m1 = vstring([0xff, 0xf0], [0x7f])
        v1 = vstring([0x88, 0x80], [0x03])
        wm.add(gate=1, priority=1, masks=m1, values=v1)
        m2 = vstring([0xf0, 0xff], [0x3f])
        v2 = vstring([0x70, 0x70], [0x05])
        wm.add(gate=2, priority=2, masks=m2, values=v2)
        wm.set_default_gate(gate=3)
        # Delivered config is sorted by priority, then gate, then mask,
        # then values. Since we use a different priority for each we can
        # just sort by priority here.
        expect_config = {
            'default_gate': 3,
            'rules': [
                {'priority': 1, 'gate': 1, 'masks': m1, 'values': v1},
                {'priority': 2, 'gate': 2, 'masks': m2, 'values': v2},
            ]
        }
        arg = pb_conv.protobuf_to_dict(wm.get_initial_arg())
        cur_config = pb_conv.protobuf_to_dict(wm.get_runtime_config())
        # import pprint
        # def pp2(*args):
        #    for a, b in zip(*[iter(args)] * 2):
        #        print('{}:'.format(a))
        #        pprint.pprint(b, indent=4)
        # pp2('iconf:', iconf, 'arg:', arg,
        #    '\nmut state:', cur_config, 'expecting:', expect_config)
        assert arg == iconf and cur_config == expect_config

suite = unittest.TestLoader().loadTestsFromTestCase(BessWildcardMatchTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
