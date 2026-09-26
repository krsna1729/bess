# Copyright (c) 2017, Joshua Stone.
# Copyright (c) 2016-2017, Nefeli Networks, Inc.
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

from test_utils import *


class BessDrrTest(BessModuleTestCase):

    def test_run_drr(self):
        buf = DRR()
        self.run_for(buf, [0], 3)
        self.assertBessAlive()

    def test_drr_single(self):
        single_basic = DRR(num_flows=2, max_flow_queue_size=100)
        single_basic.attach_task(wid=0)

        pkt = get_tcp_packet(sip='22.22.22.22', dip='22.22.22.22')
        pkt_outs = self.run_module(single_basic, 0, [pkt], [0])
        self.assertEqual(len(pkt_outs[0]), 1)
        self.assertSamePackets(pkt_outs[0][0], pkt)

    def test_drr_batch(self):
        batch_basic = DRR(num_flows=4, max_flow_queue_size=100)
        batch_basic.attach_task(wid=0)

        pkt_lists = [('22.22.22.1', '22.22.22.1'),
                     ('22.22.22.1', '22.22.22.1'),
                     ('22.22.11.1', '22.22.11.1'),
                     ('22.22.11.1', '22.22.11.1'),
                     ('22.11.11.1', '22.1.11.1')
                     ]
        for (src, dst) in pkt_lists:
            pkt = get_tcp_packet(sip=src, dip=dst)
            pkt_outs = self.run_module(batch_basic, 0, [pkt], [0])
            self.assertEqual(len(pkt_outs[0]), 1)
            self.assertSamePackets(pkt_outs[0][0], pkt)

    # Producers on two workers and the DRR task on a third, rate-limited so
    # flow queues fill and grow, with live commands throughout. Before D-019
    # the producers created, resized and freed flow queues that the task was
    # dequeuing from; a stress test cannot prove the race is gone (D-007), but
    # it did crash the old code (see the commit), and it checks that live
    # commands no longer need a pause.
    def test_drr_cross_worker(self):
        import os
        cores = sorted(os.sched_getaffinity(0))
        if len(cores) < 3:
            self.skipTest('needs three allowed CPUs')
        for wid in range(3):
            bess.add_worker(wid=wid, core=cores[wid])
        bess.add_tc('drr_rl', policy='rate_limit', wid=2, resource='packet',
                    limit={'packet': 2000000})

        drr = DRR(num_flows=1024, max_flow_queue_size=8192)
        out = Measure()
        drr -> out -> Sink()
        drr.attach_task(parent='drr_rl')
        for wid in range(2):
            src = Source()
            src -> RandomUpdate(fields=[
                {'offset': 26, 'size': 4, 'min': 1, 'max': 2000},
                {'offset': 34, 'size': 2, 'min': 1, 'max': 4}]) -> drr
            src.attach_task(wid=wid)

        bess.resume_all()
        deadline = time.time() + 3
        i = 0
        while time.time() < deadline:
            drr.set_quantum_size(quantum=1000 + (i % 1000))
            drr.set_max_flow_queue_size(max_queue_size=4096 + (i % 4096))
            i += 1
        bess.pause_all()

        self.assertBessAlive()
        self.assertGreater(out.get_summary().packets, 0)

    # Takes the number of flows n, the quantum to give drr, the list packet rates for each flow
    # and the packet rate for the module. runs this setup for five seconds and tests that
    # throughput for each flow had a jaine fairness of atleast .95.
    def _fairness_n_flow_test(n, quantum, rates, module_rate):

        bess.pause_all()

        packets = []
        exm = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4}])

        for i in range(1, n + 1):
            pkt = gen_tcp_packet()
            pkt[scapy.IP].src = '22.11.11.%d' % str(i)
            pkt[scapy.IP].dst = '22.11.11.%d' % str(i)

            packets.append(bytes(pkt))
            exm.add(
                fields=[{'value_bin': socket.inet_aton('22.11.11.' + str(i))}], gate=i)

        me_in = Measure()
        me_out = Measure()

        measrre_in = []
        measure_out = []

        q = DRR(num_flows=n + 1, quantum=quantum)
        me_in -> q -> me_out -> exm
        bess.add_tc('output', policy='rate_limit', resource='packet',
                    limit={'packet': module_rate})
        q.attach_task(parent='output')

        for i in range(0, n):
            src = Source()
            measure_in.append(Measure())
            measure_out.append(Measure())

            src -> Rewrite(templates=[packets[i]]) -> measure_in[i] -> me_in
            exm:i + 1 -> measure_out[i] -> Sink()

            bess.add_tc('r' + str(i), policy='rate_limit', resource='packet',
                        limit={'packet': rates[i]})
            src.attach_task(parent='r' + str(i))

        bess.resume_all()
        time.sleep(3)
        bess.pause_all()

        f = lambda m: (float)((m.get_summary().packets)**2)
        square_sum = 0
        for i in range(n):
            square_sum += f(measure_out[i])
        square_sum *= n

        if square_sum == 0:
            fair = 0
        else:
            fair = f(me_out) / square_sum
        self.assertTrue(abs(.99 - fair) <= .05)

    # tests the fairness of 2 and 5 flows setup using the inner helper
    # function.
    def fairness_test():

        fairness_n_flow_test(2, 1000, [80000, 20000], 30000)
        fairness_n_flow_test(
            5, 1000, [110000, 200000, 70000, 60000, 40000], 150000)

        ten_flows = [210000, 120000, 130000, 160000,
                     100000, 105000, 90000, 70000, 60000, 40000]
        fairness_n_flow_test(10, 1000, ten_flows, 300000)

suite = unittest.TestLoader().loadTestsFromTestCase(BessDrrTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
