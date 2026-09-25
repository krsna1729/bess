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


suite = unittest.TestLoader().loadTestsFromTestCase(BessApiWireRangesTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
