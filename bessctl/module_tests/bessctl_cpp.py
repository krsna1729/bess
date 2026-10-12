# SPDX-License-Identifier: BSD-3-Clause

# The C++ bessctl (G1.7) against a live daemon: a manifest whose args name no
# types is validated, planned, applied (with two rules for the ExactMatch it
# creates), read back and diffed; a bad rule names itself; a stale generation
# is refused; the schema and resources are listed.
#
# The binary is $BESSCTL_CPP (meson sets it), else the default build
# directory's; without it the test is skipped.

import json
import os
import select
import signal
import subprocess
import tempfile
from test_utils import *

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

MANIFEST = {
    'modules': [
        {'name': 'ctl_src', 'mclass': 'Source', 'arg': {'pkt_size': 60}},
        {'name': 'ctl_em', 'mclass': 'ExactMatch',
         'arg': {'fields': [{'offset': 26, 'num_bytes': 4}]}},
        {'name': 'ctl_sink', 'mclass': 'Sink'},
    ],
    'connections': [
        {'upstream': 'ctl_src', 'downstream': 'ctl_em'},
        {'upstream': 'ctl_em', 'downstream': 'ctl_sink'},
    ],
    # 10.0.0.1 -> gate 1 and 10.0.0.2 -> gate 0 (bytes in protobuf JSON are
    # base64; value_int is a decimal string or number).
    'resources': {
        'ctl_em/rules': [
            {'key': {'fields': [{'value_bin': 'CgAAAQ=='}]}, 'value': {'gate': 1}},
            {'key': {'fields': [{'value_int': '167772162'}]}, 'value': {'gate': '0'}},
        ],
    },
}


def bessctl_binary():
    return os.environ.get('BESSCTL_CPP') or os.path.join(ROOT, 'build', 'bessctl', 'bessctl')


class BessctlCppTest(BessModuleTestCase):

    def bessctl(self, *args, ok=True):
        run = subprocess.run([self.binary, '-s', self.bess.peer] + list(args),
                             capture_output=True, text=True, timeout=60)
        if ok:
            self.assertEqual(run.returncode, 0, ' '.join(args) + ': ' + run.stderr)
        return run

    def test_manifest_lifecycle(self):
        self.binary = bessctl_binary()
        if not os.access(self.binary, os.X_OK):
            self.skipTest(self.binary + ' is not built')
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'pipeline.json')
            with open(path, 'w') as f:
                json.dump(MANIFEST, f)

            normalized = json.loads(self.bessctl('validate', path).stdout)
            self.assertEqual(sorted(m['name'] for m in normalized['modules']),
                             ['ctl_em', 'ctl_sink', 'ctl_src'])

            plan = self.bessctl('plan', path).stdout
            self.assertIn('ctl_em', plan)

            applied = self.bessctl('apply', path).stdout
            self.assertTrue(applied.startswith('version '), applied)
            version = applied.split()[1].rstrip(':')  # generation@epoch
            generation, epoch = (int(x) for x in version.split('@'))
            self.assertEqual(epoch, self.bess.list_transaction_resources().daemon_epoch)
            self.assertIn('resources: 2 entries in 1 resource(s)', applied)
            rules = self.bess.run_module_command('ctl_em', 'get_runtime_config', 'EmptyArg',
                                                 {}).rules
            self.assertEqual(sorted(r.gate for r in rules), [0, 1])

            got = json.loads(self.bessctl('get').stdout)
            em = next(m for m in got['modules'] if m['name'] == 'ctl_em')
            self.assertNotIn('@type', em['arg'], 'the implied type is left out')
            self.assertEqual(em['arg']['fields'][0]['offset'], 26)
            self.assertIn('ctl_em', [m.name for m in self.bess.list_modules().modules])

            # What `get` printed applies as a no-op: nothing to change.
            again = os.path.join(tmp, 'again.json')
            with open(again, 'w') as f:
                json.dump(got, f)
            self.assertIn('0 change(s)', self.bessctl('diff', again).stdout)

            # The same as YAML (D-108): what `get -o yaml` printed is a no-op,
            # and validates to the canonical form the JSON does.
            again_yaml = os.path.join(tmp, 'again.yaml')
            with open(again_yaml, 'w') as f:
                f.write(self.bessctl('get', '-o', 'yaml').stdout)
            self.assertIn('0 change(s)', self.bessctl('diff', again_yaml).stdout)
            self.assertEqual(self.bessctl('validate', again_yaml).stdout,
                             self.bessctl('validate', again).stdout)

            # Bad entries: the pipeline (unchanged) applies, the entry is
            # named, and nothing of the transaction is committed. Entry 1 is
            # refused by bessctl (no such field), then by the daemon (two
            # fields for a one-field key).
            good = {'key': {'fields': [{'value_bin': 'CgAAAw=='}]}, 'value': {'gate': 1}}
            for entry, error in (
                    ({'key': {'fields': [{'value_bin': 'CgAAAw=='}]}, 'value': {'nope': 1}},
                     'resource "ctl_em/rules" entry 1: value: '),
                    ({'key': {'fields': [{'value_int': 1}, {'value_int': 2}]}, 'value': {}},
                     'resource "ctl_em/rules" entry 1: ')):
                bad = os.path.join(tmp, 'bad.json')
                with open(bad, 'w') as f:
                    json.dump(dict(got, resources={'ctl_em/rules': [good, entry]}), f)
                failed = self.bessctl('apply', bad, ok=False)
                self.assertEqual(failed.returncode, 1, failed.stdout)
                self.assertIn('the pipeline is applied, its resource entries are not', failed.stderr)
                self.assertIn(error, failed.stderr)
                self.assertEqual(len(self.bess.run_module_command(
                    'ctl_em', 'get_runtime_config', 'EmptyArg', {}).rules), 2)

            # A generation that is no longer active is refused, nothing changes.
            # Desired state is the whole pipeline: cut down what `get`
            # printed (workers and traffic classes stay).
            reduced = dict(got)
            reduced['modules'] = [m for m in got['modules'] if m['name'] not in
                                  ('ctl_em', 'ctl_sink')]
            reduced['connections'] = [c for c in got.get('connections', [])
                                      if 'ctl_em' not in (c['upstream'], c['downstream'])]
            with open(path, 'w') as f:
                json.dump(reduced, f)
            self.bessctl('apply', path)
            stale = self.bessctl('apply', again, '--generation', version, ok=False)
            self.assertEqual(stale.returncode, 1)
            self.assertIn('conflict', stale.stderr)
            self.assertNotIn('ctl_em', [m.name for m in self.bess.list_modules().modules])
            # The same generation of another daemon epoch: refused for the
            # epoch, before the generation is compared (D-106).
            other = '%d@%d' % (generation, epoch + 1)
            stale = self.bessctl('apply', again, '--generation', other, ok=False)
            self.assertEqual(stale.returncode, 1)
            self.assertIn('restarted', stale.stderr)
            self.assertNotIn('ctl_em', [m.name for m in self.bess.list_modules().modules])

        schema = self.bessctl('schema', 'ExactMatch').stdout
        self.assertIn('bess.pb.ExactMatchArg', schema)
        self.assertIn('bess.pb.ExactMatchCommandAddArg', schema)
        self.bessctl('status')
        self.bessctl('metrics')
        self.assertEqual(self.bessctl('nonsense', ok=False).returncode, 2)
        self.assertEqual(self.bessctl('events', '--from', 'x', ok=False).returncode, 2)
        for bad in ('-1', '5', '5@0', '5@x'):  # a generation alone is ambiguous
            self.assertEqual(self.bessctl('apply', 'f', '--generation', bad, ok=False).returncode, 2)

        # `events --from 1` replays what the daemon holds (the transactions
        # above) at once, then follows; Ctrl-C ends it cleanly. If tests before
        # this one filled the daemon's event history, sequence 1 is no longer
        # held and a bess.gap comes first.
        watch = subprocess.Popen([self.binary, '-s', self.bess.peer, 'events', '--from', '1',
                                  'bess.transaction'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            ready, _, _ = select.select([watch.stdout], [], [], 10)
            self.assertTrue(ready, 'events --from 1 printed nothing')
            first = watch.stdout.readline().split()
            if first[1] == 'bess.gap':  # replayed with what is held: already sent
                first = watch.stdout.readline().split()
            self.assertEqual(first[1], 'bess.transaction', first)
            self.assertIn('outcome=applied', first)
        finally:
            watch.send_signal(signal.SIGINT)
            _, err = watch.communicate(timeout=10)
        self.assertEqual(watch.returncode, 0, err)

    # The Admin service through bessctl (D-107): pools, ports, a refused
    # plugin, and a capture that streams pcap and is stopped on Ctrl-C.
    def test_admin(self):
        self.binary = bessctl_binary()
        if not os.access(self.binary, os.X_OK):
            self.skipTest(self.binary + ' is not built')
        pools = self.bessctl('pools').stdout
        self.assertIn('socket 0: ', pools)

        self.bess.create_port('UnixSocketPort', 'ctl_p0',
                              {'path': '@bessctl_cpp_%d' % os.getpid()})
        ports = self.bessctl('ports', 'ctl_p0').stdout
        self.assertTrue(ports.startswith('ctl_p0 (UnixSocketPort): link '), ports)
        self.assertIn('rx  packets 0', ports)
        missing = self.bessctl('ports', 'ctl_nosuch', ok=False)
        self.assertEqual(missing.returncode, 1)
        self.assertIn('ctl_nosuch', missing.stderr)

        refused = self.bessctl('plugin', 'import', '/nonexistent/plugin.so', ok=False)
        self.assertEqual(refused.returncode, 1)
        self.assertEqual(self.bessctl('plugin', 'load', 'x', ok=False).returncode, 2)

        self.bess.create_module('Bypass', 'cap_a')
        self.bess.create_module('Bypass', 'cap_b')
        self.bess.connect_modules('cap_a', 'cap_b')
        for _ in range(2):  # a stopped capture can start again
            cap = subprocess.Popen([self.binary, '-s', self.bess.peer, 'capture', 'cap_a', 'out:0'],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                ready, _, _ = select.select([cap.stdout], [], [], 10)
                self.assertTrue(ready, 'capture printed nothing')
                header = os.read(cap.stdout.fileno(), 24)
                self.assertEqual(header[:4], b'\xd4\xc3\xb2\xa1', 'a libpcap file header')
            finally:
                cap.send_signal(signal.SIGINT)
                _, err = cap.communicate(timeout=10)
            self.assertEqual(cap.returncode, 0, err)
        self.assertEqual(self.bessctl('capture', 'cap_a', 'sideways', ok=False).returncode, 2)

        # Workers: a pipeline's new worker starts paused; resume and pause
        # them all.
        self.bessctl('resume')
        self.bessctl('pause')

suite = unittest.TestLoader().loadTestsFromTestCase(BessctlCppTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
