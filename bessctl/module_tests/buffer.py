# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *

# BESS default batch size
# TODO: Any way to receive the parameter from bess daemon?
BATCH_SIZE = 32


class BessBufferTest(BessModuleTestCase):

    def test_run_buffer(self):
        buf = Buffer()
        self.run_for(buf, [0], 3)
        self.assertBessAlive()

    def test_buffer(self):
        buf = Buffer()
        pkt1 = get_tcp_packet(sip='22.22.22.22', dip='22.22.22.22')
        pkt2 = get_tcp_packet(sip='32.22.22.22', dip='22.22.22.22')

        test_data = []

        # Should withhold data until it has a full batch to push through.
        for i in range(BATCH_SIZE - 1):
            test_data.append({'input_port': 0, 'input_packet': pkt1,
                              'output_port': 0, 'output_packet': None})

        test_data.append({'input_port': 0, 'input_packet': pkt2,
                          'output_port': 0, 'output_packet': pkt1})

        for i in range(BATCH_SIZE - 1):
            pkt_outs = self.run_module(buf, 0, [pkt1], [0])
            self.assertEqual(len(pkt_outs[0]), 0)

        pkt_outs = self.run_module(buf, 0, [pkt2], [0])
        self.assertEqual(len(pkt_outs[0]), BATCH_SIZE)
        self.assertSamePackets(pkt_outs[0][0], pkt1)

suite = unittest.TestLoader().loadTestsFromTestCase(BessBufferTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
