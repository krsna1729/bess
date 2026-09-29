# Copyright (c) 2026, Nefeli Networks, Inc.
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
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
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

"""A daemon stopped with a live pipeline exits cleanly.

KillBess (`daemon stop`) used to leave worker threads running and RCU
readers registered, and static destruction then tore down the runtime's
RcuDomain before the modules publishing through it -- an abort on every
stop of a daemon with workers and an RCU-published module (found by the
live-traffic module tests, 2026-09-27). Shutdown now resets the dataplane in
order (ApiServer::Run -> ControlPlane::Reset) before exit.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))


def _bessd():
    path = os.environ.get('BESSD_BINARY')
    return path if path and os.access(path, os.X_OK) else None


@unittest.skipIf(_bessd() is None, 'BESSD_BINARY not set')
class DaemonShutdownTest(unittest.TestCase):

    PORT = 10599  # not the default: never touches another daemon

    def _start(self, port=None):
        port = self.PORT if port is None else port
        from pybess.bess import BESS
        # Unprivileged: malloc-backed packet pools (-m 0), a private pidfile.
        pidfile = tempfile.NamedTemporaryFile(suffix='.pid', delete=False)
        pidfile.close()
        self.addCleanup(os.unlink, pidfile.name)
        # A file, not a pipe: EAL logs a lot at startup, and a full pipe
        # would block the daemon.
        log = tempfile.TemporaryFile()
        self.addCleanup(log.close)
        proc = subprocess.Popen(
            [_bessd(), '-f', '-p', str(port), '--skip_root_check',
             '-m', '0', '-i', pidfile.name],
            stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: proc.poll() is None and
                        (proc.kill(), proc.wait()))
        bess = BESS()
        for _ in range(300):  # EAL setup can take several seconds
            try:
                bess.connect(grpc_url='localhost:%d' % port)
                self.addCleanup(bess.disconnect)
                return proc, log, bess
            except (bess.APIError, bess.RPCError):
                if proc.poll() is not None:
                    break
                time.sleep(0.2)
        self.fail('bessd did not come up')

    def _assert_clean_exit(self, proc, log):
        proc.wait(timeout=60)
        log.seek(0)
        text = log.read().decode(errors='replace')
        failure = [l for l in text.splitlines() if l.startswith('F')]
        self.assertEqual(proc.returncode, 0, '\n'.join(failure[:20]))
        self.assertIn('gracefully shut down', text)

    def _wildcard_rule(self, bess, op, i):
        arg = {'values': [{'value_bin': bytes([10, i % 256, 0, 0])}],
               'masks': [{'value_bin': b'\xff\xff\x00\x00'}]}
        if op == 'add':
            arg.update(gate=0, priority=1)
            bess.run_module_command('wm', 'add', 'WildcardMatchCommandAddArg',
                                    arg)
        else:
            bess.run_module_command('wm', 'delete',
                                    'WildcardMatchCommandDeleteArg', arg)

    def _unused_port(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(('127.0.0.1', 0))
            return sock.getsockname()[1]

    def test_two_instances_stop_independently(self):
        first_port = self._unused_port()
        first_proc, first_log, first_bess = self._start(first_port)
        second_port = self._unused_port()
        second_proc, second_log, second_bess = self._start(second_port)
        second_bess.create_module('Sink', 'still_here', {})

        first_bess.kill()
        self._assert_clean_exit(first_proc, first_log)
        names = [module.name for module in second_bess.list_modules().modules]
        self.assertIn('still_here', names)

        second_bess.kill()
        self._assert_clean_exit(second_proc, second_log)

    def test_stop_with_running_pipeline_exits_cleanly(self):
        proc, log, bess = self._start()
        bess.pause_all()
        bess.add_worker(0, 0)
        bess.create_module('Source', 'src', {})
        bess.create_module('WildcardMatch', 'wm',
                           {'fields': [{'offset': 26, 'num_bytes': 4}]})
        bess.create_module('Sink', 'sink', {})
        bess.connect_modules('src', 'wm')
        bess.connect_modules('wm', 'sink')
        bess.attach_task('src', wid=0)
        bess.resume_all()
        self._wildcard_rule(bess, 'add', 1)
        time.sleep(0.3)
        bess.kill()
        self._assert_clean_exit(proc, log)

    # A worker that was added but never resumed must neither hold grace
    # periods (retirements pile up otherwise; past the high-water mark the
    # control plane waited forever, external audit 2026-09-27) nor break
    # shutdown.
    def test_stop_with_a_never_resumed_worker_exits_cleanly(self):
        proc, log, bess = self._start()
        bess.pause_all()
        bess.add_worker(0, 0)
        bess.create_module('WildcardMatch', 'wm',
                           {'fields': [{'offset': 26, 'num_bytes': 4}]})
        # Rule ids are retired on delete; with the worker never resumed
        # these must still be reclaimed and the commands must not stall.
        start = time.time()
        for i in range(300):
            self._wildcard_rule(bess, 'add', i)
            self._wildcard_rule(bess, 'delete', i)
        self.assertLess(time.time() - start, 30)
        bess.kill()
        self._assert_clean_exit(proc, log)

if __name__ == '__main__':
    unittest.main()
