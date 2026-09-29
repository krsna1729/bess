# SPDX-License-Identifier: BSD-3-Clause

import errno

from test_utils import *


# The v1 API refuses wire integers that do not fit the daemon's narrower
# fields instead of narrowing them: gate 65536 must not quietly become gate 0,
# 257 queues must not become "one queue". Each rejected value wraps to a
# *valid* value, so without the check the request would succeed wrongly.
class BessApiWireRangesTest(BessModuleTestCase):

    def assertRefused(self, fn, *args):
        try:
            fn(*args)
        except BESS.Error as e:
            self.assertEqual(e.code, errno.EINVAL, e.errmsg)
            self.assertIn('out of range', e.errmsg)
            return
        self.fail('%s%s was accepted' % (fn.__name__, args))

    def test_gates_do_not_wrap(self):
        self.bess.create_module('Bypass', 'wr_up')
        self.bess.create_module('Bypass', 'wr_down')
        self.assertRefused(self.bess.connect_modules, 'wr_up', 'wr_down',
                           65536, 0)
        self.assertRefused(self.bess.connect_modules, 'wr_up', 'wr_down',
                           0, 65537)
        info = self.bess.get_module_info('wr_up')
        self.assertEqual(len(info.ogates), 0, 'nothing may be connected')

        self.bess.connect_modules('wr_up', 'wr_down', 0, 0)
        self.assertRefused(self.bess.disconnect_modules, 'wr_up', 65536)
        info = self.bess.get_module_info('wr_up')
        self.assertEqual(len(info.ogates), 1, 'gate 0 must stay connected')

    def test_queue_counts_do_not_wrap(self):
        self.assertRefused(self.bess.create_port, 'PCAPPort', 'wr_p0',
                           {'dev': 'lo', 'num_inc_q': 257})
        self.assertRefused(self.bess.create_port, 'PCAPPort', 'wr_p0',
                           {'dev': 'lo', 'num_out_q': 256})
        ports = [p.name for p in self.bess.list_ports().ports]
        self.assertNotIn('wr_p0', ports)

    def test_gate_hooks_do_not_wrap(self):
        self.bess.create_module('Bypass', 'wr_h')
        self.assertRefused(
            lambda: self.bess.track_gate(True, 'wr_track', 'wr_h',
                                         direction='in', gate=65536))
        self.assertRefused(
            lambda: self.bess.run_gatehook_command(
                'wr_track', 'wr_h', 'out', 65536, 'reset', 'EmptyArg', {}))

    def test_worker_ids_do_not_wrap(self):
        # 2^32 narrowed to int is worker 0.
        self.assertRefused(self.bess.pause_worker, 2 ** 32)
        self.assertRefused(self.bess.resume_worker, 2 ** 32)


suite = unittest.TestLoader().loadTestsFromTestCase(BessApiWireRangesTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
