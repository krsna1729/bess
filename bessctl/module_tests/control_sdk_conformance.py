# SPDX-License-Identifier: BSD-3-Clause

# The control SDK conformance suite (D-104, docs/control-sdk.md): this test
# creates an ExactMatch, runs sdk/conformance/cases.json through the C++
# driver (sdk_cpp_conformance) and the Go driver (`go test -tags live -run
# TestConformance`), requires every case to pass in each, and checks that the
# rule each committed steers packets.
#
# The C++ driver is $BESS_SDK_CPP_CONFORMANCE (meson sets it), else the default
# build directory's; a build without tests has none and that half is skipped.
# The Go command is `go`, or $BESS_GO (for example a container:
#   BESS_GO="sudo docker run --rm --network host -v $PWD:/src -w /src/sdk/go golang:1.25 go").
# Without Go the test fails unless BESS_SKIP_GO=1 (CI provides Go).

import json
import os
import shlex
import shutil
import socket
import subprocess
from test_utils import *
from builtin_pb import module_msg_pb2 as module_msg
from pybess import sdk

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CASES = os.path.join(ROOT, 'sdk', 'conformance', 'cases.json')


def case_names():
    with open(CASES) as f:
        return [c['name'] for c in json.load(f)['cases']]


def cpp_driver():
    return os.environ.get('BESS_SDK_CPP_CONFORMANCE') or os.path.join(
        ROOT, 'build', 'sdk', 'cpp', 'sdk_cpp_conformance')


def go_command():
    if os.environ.get('BESS_GO'):
        return shlex.split(os.environ['BESS_GO'])
    # A `go` on PATH that cannot run (an unconfigured version-manager shim)
    # is no toolchain.
    if shutil.which('go') and subprocess.run(['go', 'version'], capture_output=True).returncode == 0:
        return ['go']
    return None


class BessControlSdkConformanceTest(BessModuleTestCase):

    def setUp(self):
        super().setUp()
        self.em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                     {'offset': 30, 'num_bytes': 4}])
        self.em.set_default_gate(gate=3)
        self.resource = self.em.name + '/rules'
        # The rule the commit cases leave behind: 65.43.21.0 -> 12.34.56.78, gate 1.
        self.key = module_msg.ExactMatchRuleKey()
        self.key.fields.add(value_bin=socket.inet_aton('65.43.21.0'))
        self.key.fields.add(value_bin=socket.inet_aton('12.34.56.78'))

    def assert_all_passed(self, output, passed):
        missing = [n for n in case_names() if passed(n) not in output]
        self.assertFalse(missing, 'cases not passed: %s\n%s' % (missing, output))

    def assert_rule_steers_and_erase(self, who):
        pkt = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')
        outs = self.run_module(self.em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[1]), 1, 'the %s client\'s rule steers the packet' % who)
        client = sdk.Client(self.bess.peer)
        with client.transaction() as tx:
            tx.erase(client.resource(self.resource), self.key)

    def test_cpp(self):
        binary = cpp_driver()
        if not os.access(binary, os.X_OK):
            self.skipTest(binary + ' is not built (-Dbuild_tests=false)')
        run = subprocess.run([binary, self.bess.peer, self.resource, CASES],
                             capture_output=True, text=True, timeout=300)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assert_all_passed(run.stdout, lambda n: 'PASS %s\n' % n)
        self.assert_rule_steers_and_erase('C++')

    def test_go(self):
        go = go_command()
        if go is None:
            if os.environ.get('BESS_SKIP_GO') == '1':
                self.skipTest('no Go toolchain (BESS_SKIP_GO=1)')
            self.fail('no Go toolchain: install Go, set BESS_GO, or BESS_SKIP_GO=1')
        testdata = os.path.join(ROOT, 'sdk', 'go', 'bess', 'testdata')
        os.makedirs(testdata, exist_ok=True)
        config = os.path.join(testdata, 'conformance.json')
        with open(config, 'w') as f:
            json.dump({'address': self.bess.peer, 'resource': self.resource}, f)
        try:
            run = subprocess.run(go + ['test', '-count=1', '-v', '-tags', 'live',
                                       '-run', 'TestConformance', './bess'],
                                 cwd=os.path.join(ROOT, 'sdk', 'go'), capture_output=True,
                                 text=True, timeout=600)
        finally:
            os.remove(config)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assert_all_passed(run.stdout, lambda n: '--- PASS: TestConformance/%s ' % n)
        self.assert_rule_steers_and_erase('Go')


suite = unittest.TestLoader().loadTestsFromTestCase(BessControlSdkConformanceTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
