# SPDX-License-Identifier: BSD-3-Clause

# The C++ bessctl (G1.7) against a live daemon: a manifest whose args name no
# types is validated, planned, applied, read back and diffed; a stale
# generation is refused; the schema and resources are listed.
#
# The binary is $BESSCTL_CPP (meson sets it), else the default build
# directory's; without it the test is skipped.

import json
import os
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
            self.assertIn('generation', applied)
            generation = int(applied.split()[1].rstrip(':'))

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
            stale = self.bessctl('apply', again, '--generation', str(generation), ok=False)
            self.assertEqual(stale.returncode, 1)
            self.assertNotIn('ctl_em', [m.name for m in self.bess.list_modules().modules])

        schema = self.bessctl('schema', 'ExactMatch').stdout
        self.assertIn('bess.pb.ExactMatchArg', schema)
        self.assertIn('bess.pb.ExactMatchCommandAddArg', schema)
        self.bessctl('status')
        self.bessctl('metrics')
        self.assertEqual(self.bessctl('nonsense', ok=False).returncode, 2)


suite = unittest.TestLoader().loadTestsFromTestCase(BessctlCppTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
