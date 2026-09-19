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

import os
import sys
import unittest

_here = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, _here)
# protoc generates the python protobuf modules with flat imports, so both
# directories need to be importable (same as pybess/bess.py does).
sys.path.insert(0, os.path.join(_here, '..', 'pybess'))
sys.path.insert(0, os.path.join(_here, '..', 'pybess', 'builtin_pb'))

import commands  # noqa: E402

from builtin_pb import bess_msg_pb2  # noqa: E402


def _cmds(*pairs):
    return [bess_msg_pb2.CommandInfo(name=name, arg_type='EmptyArg',
                                     thread_safe=safe)
            for name, safe in pairs]


class SelectivePauseTest(unittest.TestCase):
    """bessctl pauses only for commands the daemon reports as not thread-safe.

    The decision must be conservative: everything except an explicit
    thread_safe == true means pause, because pausing is always correct and
    only the daemon can judge safety. These cover the cases where the daemon
    cannot or does not say yes.
    """

    def test_explicitly_thread_safe_skips_pause(self):
        self.assertTrue(commands._command_is_thread_safe(
            _cmds(('add', True), ('delete', False)), 'add'))

    def test_explicitly_thread_unsafe_pauses(self):
        self.assertFalse(commands._command_is_thread_safe(
            _cmds(('add', False)), 'add'))

    def test_unknown_command_pauses(self):
        self.assertFalse(commands._command_is_thread_safe(
            _cmds(('add', True)), 'resize'))

    def test_empty_metadata_pauses(self):
        # A daemon that reports no commands at all (or a response without the
        # structured field) must not be read as "everything is safe".
        self.assertFalse(commands._command_is_thread_safe([], 'add'))

    def test_cli_handlers_are_registered(self):
        # A def must not land between a @cmd() decorator and the next def:
        # cmd_decorator() registers into cmdlist and returns None, so the
        # helper would become the handler (and the real command would lose
        # its registration). This is how the selective-pause helpers were
        # first added, and it silently broke `command module` /
        # `command gatehook` until this test caught it.
        self.assertNotIn(None, [func for _, _, func in commands.cmdlist])
        for syntax, expected in (('command module ', 'command_module'),
                                 ('command gatehook ', 'command_gatehook')):
            found = [func.__name__ for s, _, func in commands.cmdlist
                     if s.startswith(syntax)]
            self.assertEqual(found, [expected], syntax)

    def test_each_command_judged_separately(self):
        cmds = _cmds(('add', True), ('delete', True), ('clear', False))
        self.assertTrue(commands._command_is_thread_safe(cmds, 'add'))
        self.assertTrue(commands._command_is_thread_safe(cmds, 'delete'))
        self.assertFalse(commands._command_is_thread_safe(cmds, 'clear'))


if __name__ == '__main__':
    unittest.main()
