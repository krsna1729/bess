# SPDX-License-Identifier: BSD-3-Clause

from test_utils import *


INSIDE = '172.16.0.2'
OUTSIDE = '8.8.8.8'


def tcp(src, dst, sport, dport, flags):
    eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
    return eth / scapy.IP(src=src, dst=dst) / \
        scapy.TCP(sport=sport, dport=dport, flags=flags) / 'x'


class BessConnTrackTest(BessModuleTestCase):
    # igate 0 (inside) may start connections, igate 1 (outside) may not;
    # tracked packets leave on ogate 0, the rest on ogate 1.

    def _stateful(self, ct):
        out = self.run_module(ct, 0, [tcp(INSIDE, OUTSIDE, 40000, 80, 'S')], [0, 1])
        self.assertEqual((len(out[0]), len(out[1])), (1, 0), 'inside SYN starts one')
        out = self.run_module(ct, 1, [tcp(OUTSIDE, INSIDE, 80, 40000, 'SA')], [0, 1])
        self.assertEqual((len(out[0]), len(out[1])), (1, 0), 'its reply is tracked')
        out = self.run_module(ct, 1, [tcp(OUTSIDE, INSIDE, 443, 40001, 'S')], [0, 1])
        self.assertEqual((len(out[0]), len(out[1])), (0, 1), 'outside SYN starts none')

    def test_owned(self):
        self._stateful(ConnTrack())

    def test_shared(self):
        self._stateful(ConnTrack(mode='SHARED'))

    def test_per_worker_on_symmetric_inputs(self):
        # The harness feeds the module from single-queue ports on one worker:
        # symmetric, so per-worker tables are allowed.
        ct = ConnTrack(mode='PER_WORKER')
        self._stateful(ct)
        self.assertIn('connections', self.bess.get_module_info(ct.name).desc)

    def test_per_worker_fails_closed_on_other_inputs(self):
        # A Source is not a port queue: no symmetric hash to rely on, so the
        # module refuses per-worker tracking and sends everything to ogate 1.
        desc = self._source_fed(ConnTrack(mode='PER_WORKER'))
        self.assertIn('closed', desc)
        self.assertIn('not a port queue', desc)


    def _source_fed(self, ct):
        src = Source()
        sink0, sink1 = Sink(), Sink()
        src -> ct
        ct:0 -> sink0
        ct:1 -> sink1
        bess.resume_all()
        bess.pause_all()
        return self.bess.get_module_info(ct.name).desc

    def test_per_worker_falls_back_to_shared(self):
        desc = self._source_fed(ConnTrack(mode='PER_WORKER', fallback_shared=True))
        self.assertIn('shared: ', desc)
        self.assertIn('not a port queue', desc)


suite = unittest.TestLoader().loadTestsFromTestCase(BessConnTrackTest)
results = unittest.TextTestRunner(verbosity=2).run(suite)

if results.failures or results.errors:
    sys.exit(1)
