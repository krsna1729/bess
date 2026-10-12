# SPDX-License-Identifier: BSD-3-Clause

# The C++ control SDK (sdk/cpp, G1.6) against a live daemon: this test creates
# an ExactMatch, hands the C++ live test its resource, key and value (and the
# descriptors to build them), runs it, and then checks that the rule the C++
# client committed steers packets. The same steps as control_sdk_go.py.
#
# The binary is $BESS_SDK_CPP_LIVE_TEST (meson sets it), else the default
# build directory's.

import os
import socket
import subprocess
import tempfile
from test_utils import *
from builtin_pb import module_msg_pb2 as module_msg
from pybess import sdk

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def live_test_binary():
    path = os.environ.get('BESS_SDK_CPP_LIVE_TEST')
    if path:
        return path
    return os.path.join(ROOT, 'build', 'sdk', 'cpp', 'sdk_cpp_live_test')


class BessControlSdkCppTest(BessModuleTestCase):

    def test_cpp_sdk_commits_a_rule(self):
        binary = live_test_binary()
        self.assertTrue(os.access(binary, os.X_OK), binary + ' is not built')
        em = ExactMatch(fields=[{'offset': 26, 'num_bytes': 4},
                                {'offset': 30, 'num_bytes': 4}])
        em.set_default_gate(gate=3)
        key = module_msg.ExactMatchRuleKey()
        key.fields.add(value_bin=socket.inet_aton('65.43.21.0'))
        key.fields.add(value_bin=socket.inet_aton('12.34.56.78'))
        value = module_msg.ExactMatchRuleValue(gate=1)
        with tempfile.TemporaryDirectory() as tmp:
            descriptors = os.path.join(tmp, 'set.pb')
            subprocess.check_call(['protoc', '-I', os.path.join(ROOT, 'protobuf'),
                                   '--include_imports', '--descriptor_set_out=' + descriptors,
                                   'module_msg.proto'])
            key_file = os.path.join(tmp, 'key')
            value_file = os.path.join(tmp, 'value')
            with open(key_file, 'wb') as f:
                f.write(key.SerializeToString())
            with open(value_file, 'wb') as f:
                f.write(value.SerializeToString())
            run = subprocess.run([binary, self.bess.peer, em.name + '/rules', descriptors,
                                  key_file, value_file],
                                 capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

        pkt = get_tcp_packet(sip='65.43.21.0', dip='12.34.56.78')
        outs = self.run_module(em, 0, [pkt], [0, 1, 2, 3])
        self.assertEqual(len(outs[1]), 1, 'the C++ client\'s rule steers the packet')

        client = sdk.Client(self.bess.peer)
        with client.transaction() as tx:
            tx.erase(client.resource(em.name + '/rules'), key)


suite = unittest.TestLoader().loadTestsFromTestCase(BessControlSdkCppTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
