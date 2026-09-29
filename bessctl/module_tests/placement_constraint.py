# Copyright (c) 2017, The Regents of the University of California.
# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

import os

from test_utils import *


class BessModuleConstraintTest(BessModuleTestCase):

    def test_queue(self):
        # This is taken from queue.bess
        src = Source()
        src -> queue::Queue() \
            -> VLANPush(tci=2) \
            -> Sink()

        bess.add_tc('fast', policy='rate_limit',
                    resource='packet', limit={'packet': 9000000})
        src.attach_task('fast')

        bess.add_tc('slow', policy='rate_limit',
                    resource='packet', limit={'packet': 1000000})
        queue.attach_task('slow')

        self.assertFalse(bess.check_constraints())

    def test_nat(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        # From nat.bess -- check that revisiting the same module works
        # correctly.
        nat = NAT(ext_addrs=nat_config)

        # Swap src/dst MAC
        mac = MACSwap()

        # Swap src/dst IP addresses / ports
        ip = IPSwap()

        Source() -> 0:nat:1 -> mac -> ip -> 1:nat:0 -> Sink()

        self.assertFalse(bess.check_constraints())

    def test_nat_queue(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        # Check a combination.
        nat = NAT(ext_addrs=nat_config)

        # Swap src/dst IP addresses / ports
        ip = IPSwap()

        Source() -> 0:nat:1 -> Queue() -> ip -> 1:nat:0 -> Sink()

        self.assertFalse(bess.check_constraints())

    def test_nat_negative(self):
        nat_config = [{'ext_addr': '192.168.1.1'}]
        src0 = Source()
        src1 = Source()
        cores = sorted(os.sched_getaffinity(0))
        if len(cores) < 2:
            self.skipTest("requires at least two available CPU cores")
        bess.add_worker(0, cores[0])
        bess.add_worker(1, cores[1])
        nat = NAT(ext_addrs=nat_config)
        src0 -> 0: nat: 1 -> Sink()
        src1 -> 1: nat: 0 -> Sink()
        src0.attach_task(wid=0)
        src1.attach_task(wid=1)

        with self.assertRaises(bess.ConstraintError):
            bess.check_constraints()


suite = unittest.TestLoader().loadTestsFromTestCase(BessModuleConstraintTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
