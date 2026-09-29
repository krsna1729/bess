# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

import os
from test_utils import *


class BessWorkerSplitTest(BessModuleTestCase):

    def test_worker_split_default(self):
        worker_cores = sorted(os.sched_getaffinity(0))
        if len(worker_cores) < 2:
            self.skipTest('worker split requires at least two allowed CPUs')
        NUM_WORKERS = min(2, len(worker_cores), 64)

        for wid in range(NUM_WORKERS):
            for i in range(NUM_WORKERS):
                bess.add_worker(wid=i, core=worker_cores[i])

            src = Source()
            ws = WorkerSplit()
            src -> ws

            for i in range(NUM_WORKERS):
                ws:i -> Sink()

            src.attach_task(wid=wid)

            bess.resume_all()
            time.sleep(1)
            bess.pause_all()

            # packets should flow onto only one output gate...
            ogates = bess.get_module_info(ws.name).ogates
            for ogate in ogates:
                if ogate.ogate == wid:
                    self.assertGreater(ogate.pkts, 0)
                else:
                    self.assertEqual(ogate.pkts, 0)

            bess.reset_all()

    def test_worker_split_fancy(self):
        worker_cores = sorted(os.sched_getaffinity(0))
        if len(worker_cores) < 2:
            self.skipTest('worker split requires at least two allowed CPUs')
        NUM_WORKERS = min(len(worker_cores), 64)

        gates = dict()
        for i in range(NUM_WORKERS):
            gates[i] = i & 1

        for i in range(NUM_WORKERS):
            bess.add_worker(wid=i, core=worker_cores[i])
            bess.add_tc('rl_{}'.format(i), policy='rate_limit', wid=i,
                        resource='count', limit={'count': 1})
        bess.pause_all()

        ws::WorkerSplit(worker_gates=gates)
        ws:0 -> sink0::Sink()
        ws:1 -> sink1::Sink()

        srcs = [Source() for _ in range(NUM_WORKERS)]
        for i in range(NUM_WORKERS):
            srcs[i].attach_task(parent='rl_{}'.format(i))
            srcs[i] -> ws

        bess.resume_all()
        time.sleep(3)
        bess.pause_all()

        odd_pkts = 0
        even_pkts = 0
        for i in range(NUM_WORKERS):
            pkts = bess.get_module_info(srcs[i].name).ogates[0].pkts
            if i % 2 == 0:
                even_pkts += pkts
            else:
                odd_pkts += pkts

        ws_ogates = bess.get_module_info(ws.name).ogates
        for ogate in ws_ogates:
            if ogate.ogate == 0:
                self.assertEqual(ogate.pkts, even_pkts)
            elif ogate.ogate == 1:
                self.assertEqual(ogate.pkts, odd_pkts)

        bess.reset_all()

suite = unittest.TestLoader().loadTestsFromTestCase(BessWorkerSplitTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
