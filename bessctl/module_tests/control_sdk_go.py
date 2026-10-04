# SPDX-License-Identifier: BSD-3-Clause

# The Go control SDK (sdk/go, M27) against a live daemon: this test creates
# an ExactMatch, hands the Go test its resource, key and value (and the
# descriptors to build them), runs `go test -tags live`, and then checks that
# the rule the Go client committed steers packets.
#
# The Go command is `go`, or $BESS_GO (for example a container:
#   BESS_GO="sudo docker run --rm --network host -v $PWD:/src -w /src/sdk/go golang:1.25 go").
# Without Go the test fails unless BESS_SKIP_GO=1 (CI provides Go).

import base64
import json
import os
import shlex
import shutil
import socket
import subprocess
import tempfile
from test_utils import *
from builtin_pb import module_msg_pb2 as module_msg
from pybess import sdk

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def go_command():
    if os.environ.get('BESS_GO'):
        return shlex.split(os.environ['BESS_GO'])
    # A `go` on PATH that cannot run (an unconfigured version-manager shim)
    # is no toolchain.
    if shutil.which('go') and subprocess.run(['go', 'version'], capture_output=True).returncode == 0:
        return ['go']
    return None


def descriptor_set():
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'set.pb')
        subprocess.check_call(['protoc', '-I', os.path.join(ROOT, 'protobuf'), '--include_imports',
                               '--descriptor_set_out=' + out, 'module_msg.proto'])
        with open(out, 'rb') as f:
            return f.read()


class BessControlSdkGoTest(BessModuleTestCase):

    def test_go_sdk_commits_a_rule(self):
        go = go_command()
        if go is None:
            if os.environ.get('BESS_SKIP_GO') == '1':
                self.skipTest('no Go toolchain (BESS_SKIP_GO=1)')
            self.fail('no Go toolchain: install Go, set BESS_GO, or BESS_SKIP_GO=1')
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                {'offset': 30, 'num_bytes': 4}])
        em.set_default_gate(gate=3)
        key = module_msg.ExactMatchRuleKey()
        key.fields.add(value_bin=socket.inet_aton('65.43.21.0'))
        key.fields.add(value_bin=socket.inet_aton('12.34.56.78'))
        value = module_msg.ExactMatchRuleValue(gate=1)
        config = {
            'address': self.bess.peer,
            'resource': em.name + '/rules',
            'descriptors': base64.b64encode(descriptor_set()).decode(),
            'key': base64.b64encode(key.SerializeToString()).decode(),
            'value': base64.b64encode(value.SerializeToString()).decode(),
        }
        testdata = os.path.join(ROOT, 'sdk', 'go', 'bess', 'testdata')
        os.makedirs(testdata, exist_ok=True)
        path = os.path.join(testdata, 'live.json')
        with open(path, 'w') as f:
            json.dump(config, f)
        try:
            run = subprocess.run(go + ['test', '-count=1', '-tags', 'live', '-run', 'TestLive',
                                       './bess'],
                                 cwd=os.path.join(ROOT, 'sdk', 'go'), capture_output=True, text=True)
        finally:
            os.remove(path)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

        pkt = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[1]), 1, 'the Go client\'s rule steers the packet')

        client = sdk.Client(self.bess.peer)
        with client.transaction() as tx:
            tx.erase(client.resource(em.name + '/rules'), key)


suite = unittest.TestLoader().loadTestsFromTestCase(BessControlSdkGoTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
