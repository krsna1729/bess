# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


class BessL2ForwardTest(BessModuleTestCase):

    def test_l2forward(self):
        l2fib = L2Forward()

        l2fib.add(entries=[{'addr': '00:01:02:03:04:05', 'gate': 64},
                           {'addr': 'aa:bb:cc:dd:ee:ff', 'gate': 1},
                           {'addr': '11:11:11:11:11:22', 'gate': 2}])
        with self.assertRaises(bess.Error):
            l2fib.add(entries=[{'addr': '00:01:02:03:04:05', 'gate': 0}])

        ret = l2fib.lookup(addrs=['aa:bb:cc:dd:ee:ff', '00:01:02:03:04:05'])
        self.assertEqual(ret.gates, [1, 64])

        l2fib.delete(addrs=['00:01:02:03:04:05'])
        with self.assertRaises(bess.Error):
            l2fib.delete(addrs=['00:01:02:03:04:05'])

    # Commands run while workers keep processing (G1.2 mode C) and validate
    # everything before applying anything: int64 wire gates are checked
    # before narrowing, a bad entry adds nothing, and populate with
    # gate_count 0 is refused (it used to divide by zero and crash bessd).
    def test_l2forward_validation(self):
        l2fib = L2Forward()
        for gate in (65536, -1, 8193):
            with self.assertRaises(bess.Error):
                l2fib.add(entries=[{'addr': '00:01:02:03:04:05',
                                    'gate': gate}])
        with self.assertRaises(bess.Error):
            l2fib.add(entries=[{'addr': '00:01:02:03:04:06', 'gate': 1},
                               {'addr': 'not-a-mac', 'gate': 2}])
        with self.assertRaises(bess.Error):
            l2fib.lookup(addrs=['00:01:02:03:04:06'])  # nothing was added
        with self.assertRaises(bess.Error):
            l2fib.add(entries=[{'addr': '00:01:02:03:04:07', 'gate': 1},
                               {'addr': '00:01:02:03:04:07', 'gate': 2}])
        with self.assertRaises(bess.Error):
            l2fib.populate(base='00:01:02:03:00:00', count=10, gate_count=0)
        self.assertBessAlive()
        with self.assertRaises(bess.Error):
            l2fib.set_default_gate(gate=65536)
        l2fib.add(entries=[{'addr': '00:01:02:03:04:08', 'gate': 5}])
        self.assertEqual(list(l2fib.lookup(addrs=['00:01:02:03:04:08']).gates),
                         [5])

    # set_default_gate is THREAD_SAFE: it runs while a worker forwards
    # traffic, and the worker reads the gate atomically. Packets from unknown
    # MACs must follow each new default gate.
    def test_l2forward_default_gate_live(self):
        src = Source()
        l2 = L2Forward()
        src -> l2
        l2:0 -> Sink()
        l2:1 -> Sink()
        src.attach_task(wid=0)
        bess.resume_all()
        deadline = time.time() + 2
        i = 0
        while time.time() < deadline:
            l2.set_default_gate(gate=i % 2)
            i += 1
        bess.pause_all()
        self.assertBessAlive()
        pkts = {g.ogate: g.pkts for g in bess.get_module_info(l2.name).ogates}
        self.assertGreater(pkts.get(0, 0), 0)
        self.assertGreater(pkts.get(1, 0), 0)

    # Size 1 used to compute an alternate bucket far outside the table, and
    # int64 sizes narrowed to int (2^32 + 1 became 1).
    def test_l2forward_table_size_limits(self):
        for bad in [{'size': 1}, {'size': 2 ** 32 + 1}, {'size': -4},
                    {'bucket': 2 ** 32 + 4}, {'size': 6}]:
            with self.assertRaises(bess.Error, msg=str(bad)):
                L2Forward(**bad)
        l2 = L2Forward(size=2, bucket=4)
        l2.add(entries=[{'addr': '02:00:00:00:00:01', 'gate': 3}])
        self.assertEqual(
            list(l2.lookup(addrs=['02:00:00:00:00:01']).gates), [3])
        self.assertBessAlive()

    # populate starts at `base` (an extra shift used to start two bytes
    # lower), cycles gates, and refuses more entries than the table has slots.
    def test_l2forward_populate_addresses(self):
        l2 = L2Forward(size=64, bucket=4)
        l2.populate(base='aa:bb:cc:dd:ee:fe', count=4, gate_count=3)
        gates = l2.lookup(addrs=['aa:bb:cc:dd:ee:fe', 'aa:bb:cc:dd:ee:ff',
                                 'aa:bb:cc:dd:ef:00', 'aa:bb:cc:dd:ef:01']).gates
        self.assertEqual(list(gates), [0, 1, 2, 0])
        with self.assertRaises(bess.Error):
            l2.lookup(addrs=['00:00:aa:bb:cc:dd'])
        with self.assertRaises(bess.Error):
            l2.populate(base='02:00:00:00:00:00', count=64 * 4 + 1,
                        gate_count=1)

    # A multi-entry add that runs out of space takes back what it added.
    def test_l2forward_add_rolls_back_when_full(self):
        l2 = L2Forward(size=2, bucket=1)  # two slots
        entries = [{'addr': '02:00:00:00:00:%02x' % i, 'gate': i}
                   for i in range(1, 6)]
        with self.assertRaises(bess.Error):
            l2.add(entries=entries)
        for e in entries:
            with self.assertRaises(bess.Error, msg=e['addr']):
                l2.lookup(addrs=[e['addr']])

    # add/delete run while a worker forwards traffic (G1.2 mode C).
    def test_l2forward_add_delete_live(self):
        l2 = L2Forward(size=1024, bucket=4)
        l2.set_default_gate(gate=0)

        def command(i):
            addr = '02:00:00:00:%02x:%02x' % ((i // 2) // 256 % 256,
                                              (i // 2) % 256)
            if i % 2 == 0:
                l2.add(entries=[{'addr': addr, 'gate': 1}])
            else:
                l2.delete(addrs=[addr])

        pkts = self.run_with_live_commands(l2, [0, 1], command)
        self.assertGreater(sum(pkts.values()), 0)


suite = unittest.TestLoader().loadTestsFromTestCase(BessL2ForwardTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
