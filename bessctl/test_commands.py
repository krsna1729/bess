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
from types import SimpleNamespace

_here = os.path.dirname(os.path.realpath(__file__))
# commands.py imports its sibling `sugar` by name, so bessctl/ has to be
# importable. One normalized entry, added only when missing: an unnormalized
# duplicate (:bessctl/../pybess) makes pybess/bess.py's plugin scanner visit
# the same directory twice and report duplicate protobuf definitions.
if _here not in sys.path:
    sys.path.insert(0, _here)

import commands  # noqa: E402

import pybess.module  # noqa: E402


def _cmd_infos(*pairs):
    return [SimpleNamespace(name=name, arg_type='EmptyArg', thread_safe=safe)
            for name, safe in pairs]


class _FakeHook:
    """Stands in for GateHookInfo, including the proto3 detail that the scalar
    the `gate` oneof did *not* set still reads as 0."""

    def __init__(self, hook_name, module_name, gate_field, gate,
                 class_name='Track'):
        self.hook_name = hook_name
        self.module_name = module_name
        self.class_name = class_name
        self._gate_field = gate_field
        self._gate = gate

    def WhichOneof(self, name):
        return self._gate_field if name == 'gate' else None

    def __getattr__(self, item):
        if item == self._gate_field:
            return self._gate
        if item in ('igate', 'ogate'):
            return 0
        raise AttributeError(item)


class _FakeBess:
    def __init__(self, hooks=(), cmds_by_class=None):
        self.hooks = list(hooks)
        self.cmds_by_class = cmds_by_class or {}

    def list_gatehooks(self):
        return SimpleNamespace(hooks=self.hooks)

    def get_gatehook_class_info(self, class_name):
        return SimpleNamespace(cmds=self.cmds_by_class.get(class_name, []))


class SelectivePauseTest(unittest.TestCase):
    """bessctl pauses only for commands the daemon reports as not thread-safe.

    The decision must be conservative: everything except an explicit
    thread_safe == true means pause, because pausing is always correct and
    only the daemon can judge safety.
    """

    def test_explicitly_thread_safe_skips_pause(self):
        self.assertTrue(commands._command_is_thread_safe(
            _cmd_infos(('add', True), ('delete', False)), 'add'))

    def test_explicitly_thread_unsafe_pauses(self):
        self.assertFalse(commands._command_is_thread_safe(
            _cmd_infos(('add', False)), 'add'))

    def test_unknown_command_pauses(self):
        self.assertFalse(commands._command_is_thread_safe(
            _cmd_infos(('add', True)), 'resize'))

    def test_empty_metadata_pauses(self):
        # A daemon that reports no commands at all must not be read as
        # "everything is safe".
        self.assertFalse(commands._command_is_thread_safe([], 'add'))

    def test_each_command_judged_separately(self):
        cmds = _cmd_infos(('add', True), ('delete', True), ('clear', False))
        self.assertTrue(commands._command_is_thread_safe(cmds, 'add'))
        self.assertTrue(commands._command_is_thread_safe(cmds, 'delete'))
        self.assertFalse(commands._command_is_thread_safe(cmds, 'clear'))

    def test_cli_handlers_are_registered(self):
        # A def must not land between a @cmd() decorator and the next def:
        # cmd_decorator() registers into cmdlist and returns None, so a helper
        # would become the handler (and the real command would lose its
        # registration). This is how the selective-pause helpers were first
        # added, and it silently broke `command module`/`command gatehook`.
        self.assertNotIn(None, [func for _, _, func in commands.cmdlist])
        for syntax, expected in (('command module ', 'command_module'),
                                 ('command gatehook ', 'command_gatehook')):
            found = [func.__name__ for s, _, func in commands.cmdlist
                     if s.startswith(syntax)]
            self.assertEqual(found, [expected], syntax)


class GateHookResolutionTest(unittest.TestCase):
    """The gatehook lookup must use direction as part of the hook's identity.

    The same hook name can sit on an input and an output gate, and an unset
    oneof scalar reads as 0 -- so matching on the gate index alone resolves
    input hooks as if they were output hooks.
    """

    def _cli(self, hooks):
        # Safety is reported per class, so which hook was resolved is visible.
        return SimpleNamespace(bess=_FakeBess(hooks, cmds_by_class={
            'InClass': _cmd_infos(('reset', True)),
            'OutClass': _cmd_infos(('reset', False)),
        }))

    def test_input_gate_zero_resolves_the_input_hook(self):
        cli = self._cli([_FakeHook('t', 'ipl', 'igate', 0, 'InClass')])
        self.assertTrue(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'in', 0, 'reset'))

    def test_input_hook_is_not_resolved_for_output(self):
        # The pitfall: igate was set, so ogate reads as 0 and used to match.
        cli = self._cli([_FakeHook('t', 'ipl', 'igate', 0, 'InClass')])
        self.assertFalse(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'out', 0, 'reset'))

    def test_output_gate_zero_resolves_the_output_hook(self):
        cli = self._cli([_FakeHook('t', 'ipl', 'ogate', 0, 'OutClass')])
        self.assertFalse(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'out', 0, 'reset'))

    def test_nonzero_gate_matches_only_that_gate(self):
        cli = self._cli([_FakeHook('t', 'ipl', 'ogate', 3, 'OutClass')])
        self.assertFalse(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'out', 3, 'reset'))
        self.assertFalse(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'out', 0, 'reset'))

    def test_same_hook_name_on_both_directions(self):
        cli = self._cli([_FakeHook('t', 'ipl', 'igate', 0, 'InClass'),
                         _FakeHook('t', 'ipl', 'ogate', 0, 'OutClass')])
        self.assertTrue(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'in', 0, 'reset'))
        self.assertFalse(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'out', 0, 'reset'))

    def test_unknown_direction_is_not_guessed(self):
        cli = self._cli([_FakeHook('t', 'ipl', 'igate', 0, 'InClass')])
        self.assertFalse(commands._gatehook_command_is_thread_safe(
            cli, 't', 'ipl', 'sideways', 0, 'reset'))


class PybessWrapperTest(unittest.TestCase):
    """pybess installs one method per advertised command, named by cmd.name."""

    class _FakeModuleBess(_FakeBess):
        def __init__(self, cmds):
            super().__init__()
            self._cmds = cmds

        def create_module(self, mclass, name, arg):
            return SimpleNamespace(name=name or 'mod0')

        def get_mclass_info(self, mclass):
            return SimpleNamespace(cmds=self._cmds)

        def run_module_command(self, name, cmd, arg_type, args):
            return 'ran %s' % cmd

    def _module_class(self, cmds):
        bess = self._FakeModuleBess(cmds)

        class Wrapped(pybess.module.Module):
            # the generated module classes provide this; we only exercise the
            # command-method installation that follows it
            choose_arg = staticmethod(lambda name, kwargs: kwargs)

        Wrapped.bess = bess
        return Wrapped

    def test_methods_are_installed_under_the_command_name(self):
        mod = self._module_class(_cmd_infos(('add', True)))()
        self.assertTrue(callable(mod.add))
        self.assertEqual(mod.add(), 'ran add')

    def test_command_info_object_is_not_used_as_the_attribute_name(self):
        # Regression: `setattr(self, cmd, ...)` passed the CommandInfo object
        # itself and raised TypeError on every module construction.
        cmds = _cmd_infos(('set_default_gate', True), ('clear', True))
        mod = self._module_class(cmds)()
        for name in ('set_default_gate', 'clear'):
            self.assertTrue(callable(getattr(mod, name)), name)
        self.assertNotIn('CommandInfo', dir(mod))


if __name__ == '__main__':
    unittest.main()
