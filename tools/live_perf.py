#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Classified live performance tests: which part of BESS got slower.

Each test is a small pipeline on one pinned worker that isolates one cost.
The result is ns per packet (the worker's leaf traffic classes' packet
counters, so nothing is added to the packet path). Classes:

  F  framework, cross-cutting -- every pipeline pays it:
       F.floor          Source -> Sink
       F.hop/N          Source -> N x Bypass -> Sink      (slope: cost per module hop)
       F.fanout/N       Source -> RoundRobin(N gates) -> N Sinks   (gate fan-out)
       F.fanin/N        N x Source -> one Sink (merge)
       F.hook           F.hop/1 with a Track hook on every gate (gate-hook cost)
       F.tc/N           N leaf traffic classes under round_robin (scheduler cost)
       F.size/B         F.floor with B-byte packets (allocation and copy)
       F.queue          Source -> Queue | worker 1: Queue -> Sink (cross-worker handoff)
  M  module, localised -- ns per packet *above* M.base (Source -> Rewrite -> Bypass -> Sink,
       the same headers and flow mix every M test uses):
       M.exactmatch, M.wildcardmatch, M.iplookup, M.l2forward, M.acl, M.hashlb,
       M.update, M.nat, M.vlan
  Reading a result: F.floor or F.hop moving moves every pipeline (cross-cutting);
  only one M delta moving is that module (localised).

Runs: two (or more) builds, palindromic rounds (A B ... B A), each round a fresh
bessd running every selected test, under `omarchy-benchmark --isolate`:

  tools/live_perf.py --build master --build current:/path/to/tree --rounds 4 --cpu 2,4

`--save results.json` keeps the medians; `--baseline results.json --threshold 5`
compares the last build against a saved run and exits 1 when any test is slower
by more than the threshold (a regression gate).
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
MASTER = Path('/var/tmp/bess-clawback-base-20260930')
MASTER_TOOLCHAIN = Path('/var/tmp/master-bench-toolchain')
DPDK_LIB = Path(os.environ.get('BESS_DPDK_LIB',
                             '/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib'))


# -- packets --------------------------------------------------------------------

def ipv4_udp(src=0x0a000001, dst=0x0b000001, sport=40000, dport=2000, size=60, vlan=False):
    """An Ethernet/IPv4/UDP frame of `size` bytes (checksum filled). The source
    port is unprivileged: NAT maps a port below 1024 only into a range below
    1024 (RFC 4787 REQ-5-a), and M.nat's range starts at 1024."""
    eth = bytes.fromhex('020000000002' '020000000001') + (b'\x81\x00\x00\x01' if vlan else b'')
    eth += b'\x08\x00'
    payload = max(0, size - len(eth) - 28)
    total = 28 + payload
    ip = struct.pack('!BBHHHBBHII', 0x45, 0, total, 0, 0, 64, 17, 0, src, dst)
    csum = sum(struct.unpack('!10H', ip))
    csum = (csum & 0xffff) + (csum >> 16)
    csum = ~((csum & 0xffff) + (csum >> 16)) & 0xffff
    ip = ip[:10] + struct.pack('!H', csum) + ip[12:]
    udp = struct.pack('!HHHH', sport, dport, 8 + payload, 0)
    return eth + ip + udp + bytes(payload)


# -- the catalogue ----------------------------------------------------------------
# build(b) creates the pipeline on worker 0 (b.w0) and returns the module
# names whose leaf traffic classes count the measured packets.

FLOWS = 1024  # source addresses the M tests cycle through (RandomUpdate)


def _source(b, name='src', size=60, template=None):
    b.mod('Source', name, {'pkt_size': size})
    if template is not None:
        b.mod('Rewrite', name + '_rw', {'templates': [template]})
        b.conn(name, name + '_rw')
        return name + '_rw'
    return name


def f_floor(b, size=60):
    b.mod('Sink', 'sink', {})
    b.conn(_source(b, size=size), 'sink')
    return ['src']


def f_hop(n):
    def build(b):
        last = _source(b)
        for i in range(n):
            b.mod('Bypass', 'bp%d' % i, {})
            b.conn(last, 'bp%d' % i)
            last = 'bp%d' % i
        b.mod('Sink', 'sink', {})
        b.conn(last, 'sink')
        return ['src']
    return build


def f_hook(b):
    names = f_hop(1)(b)
    for m in ('bp0', 'sink'):
        b.bess.track_gate(True, '', m, direction='in')
    b.bess.track_gate(True, '', 'bp0', direction='out')
    return names


def f_fanout(n):
    def build(b):
        b.mod('RoundRobin', 'rr', {'gates': list(range(n))})
        b.conn(_source(b), 'rr')
        for g in range(n):
            b.mod('Sink', 'sink%d' % g, {})
            b.conn('rr', 'sink%d' % g, g)
        return ['src']
    return build


def f_fanin(n):
    def build(b):
        b.mod('Merge', 'merge', {})
        b.mod('Sink', 'sink', {})
        b.conn('merge', 'sink')
        for i in range(n):
            b.mod('Source', 'src%d' % i, {'pkt_size': 60})
            b.conn('src%d' % i, 'merge', 0, i)
        return ['src%d' % i for i in range(n)]
    return build


def f_tc(n):
    def build(b):
        b.bess.add_tc('rr_root', policy='round_robin', wid=0)
        names = []
        for i in range(n):
            b.mod('Source', 'src%d' % i, {'pkt_size': 60})
            b.mod('Sink', 'sink%d' % i, {})
            b.conn('src%d' % i, 'sink%d' % i)
            b.bess.attach_task('src%d' % i, parent='rr_root')
            names.append('src%d' % i)
        return names
    return build


def f_queue(b):
    b.mod('Queue', 'q', {'size': 1024})
    b.mod('Sink', 'sink', {})
    b.conn(_source(b), 'q')
    b.conn('q', 'sink')
    b.bess.attach_task('q', wid=1)
    return ['q']


def m_base(b, rest=None, template=None):
    """Source -> Rewrite -> RandomUpdate (FLOWS source addresses) -> [rest] -> Sink."""
    last = _source(b, template=template or ipv4_udp())
    b.mod('RandomUpdate', 'ru', {'fields': [{'offset': 26, 'size': 4, 'min': 0x0a000001,
                                             'max': 0x0a000000 + FLOWS}]})
    b.conn(last, 'ru')
    last = 'ru'
    for name, gates in (rest or [('bp', 1)]):
        if name == 'bp':
            b.mod('Bypass', 'bp', {})
        b.conn(last, name)
        last = name
        if gates > 1:
            for g in range(gates):
                b.mod('Sink', 'sink%d' % g, {})
                b.conn(name, 'sink%d' % g, g)
            return ['src']
    b.mod('Sink', 'sink', {})
    b.conn(last, 'sink')
    return ['src']


def m_exactmatch(b):
    b.mod('ExactMatch', 'em', {'fields': [{'offset': 26, 'num_bytes': 4}]})
    b.cmd('em', 'set_default_gate', 'ExactMatchCommandSetDefaultGateArg', {'gate': 0})
    for i in range(FLOWS):
        b.cmd('em', 'add', 'ExactMatchCommandAddArg',
              {'gate': 1, 'fields': [{'value_bin': struct.pack('!I', 0x0a000001 + i)}]})
    return m_base(b, [('em', 2)])


def m_wildcardmatch(b):
    b.mod('WildcardMatch', 'wm', {'fields': [{'offset': 26, 'num_bytes': 4},
                                             {'offset': 30, 'num_bytes': 4}]})
    b.cmd('wm', 'set_default_gate', 'WildcardMatchCommandSetDefaultGateArg', {'gate': 0})
    for i, mask in enumerate((0xffffff00, 0xffff0000, 0xff000000, 0xfffff000)):
        b.cmd('wm', 'add', 'WildcardMatchCommandAddArg',
              {'gate': 1, 'priority': i,
               'values': [{'value_int': 0x0a000000 & mask}, {'value_int': 0x0b000000}],
               'masks': [{'value_int': mask}, {'value_int': 0xff000000}]})
    return m_base(b, [('wm', 2)])


def m_iplookup(b):
    b.mod('IPLookup', 'ipl', {})
    for i in range(256):
        b.cmd('ipl', 'add', 'IPLookupCommandAddArg',
              {'prefix': '11.%d.0.0' % i, 'prefix_len': 16, 'gate': 1})
    b.cmd('ipl', 'add', 'IPLookupCommandAddArg', {'prefix': '0.0.0.0', 'prefix_len': 0, 'gate': 0})
    return m_base(b, [('ipl', 2)])


def m_l2forward(b):
    b.mod('L2Forward', 'l2', {})
    b.cmd('l2', 'add', 'L2ForwardCommandAddArg',
          {'entries': [{'addr': '02:00:00:00:00:02', 'gate': 1}]})
    b.cmd('l2', 'set_default_gate', 'L2ForwardCommandSetDefaultGateArg', {'gate': 0})
    return m_base(b, [('l2', 2)])


def m_acl(b):
    rules = [{'src_ip': '10.0.%d.0/24' % i, 'dst_ip': '11.0.0.0/8', 'drop': False}
             for i in range(16)]
    b.mod('ACL', 'acl', {'rules': rules})
    return m_base(b, [('acl', 1)])


def m_hashlb(b):
    b.mod('HashLB', 'lb', {'gates': [0, 1, 2, 3], 'mode': 'l3'})
    return m_base(b, [('lb', 4)])


def m_update(b):
    b.mod('Update', 'up', {'fields': [{'offset': 14 + 8, 'size': 1, 'value': 32},
                                      {'offset': 0, 'size': 6, 'value': 0x020000000003}]})
    return m_base(b, [('up', 1)])


def m_nat(b):
    b.mod('NAT', 'nat', {'ext_addrs': [{'ext_addr': '192.0.2.1',
                                        'port_ranges': [{'begin': 1024, 'end': 65535}]}]})
    return m_base(b, [('nat', 2)])


def m_vlan(b):
    b.mod('VLANPush', 'vpush', {'tci': 7})
    b.mod('VLANPop', 'vpop', {})
    return m_base(b, [('vpush', 1), ('vpop', 1)])


TESTS = {
    'F.floor': ('F', f_floor),
    'F.hop/1': ('F', f_hop(1)),
    'F.hop/4': ('F', f_hop(4)),
    'F.hop/16': ('F', f_hop(16)),
    'F.hook': ('F', f_hook),
    'F.fanout/4': ('F', f_fanout(4)),
    'F.fanin/4': ('F', f_fanin(4)),
    'F.tc/1': ('F', f_tc(1)),
    'F.tc/16': ('F', f_tc(16)),
    'F.size/1500': ('F', lambda b: f_floor(b, 1500)),
    'F.queue': ('F', f_queue),
    'M.base': ('M', m_base),
    'M.exactmatch': ('M', m_exactmatch),
    'M.wildcardmatch': ('M', m_wildcardmatch),
    'M.iplookup': ('M', m_iplookup),
    'M.l2forward': ('M', m_l2forward),
    'M.acl': ('M', m_acl),
    'M.hashlb': ('M', m_hashlb),
    'M.update': ('M', m_update),
    'M.nat': ('M', m_nat),
    'M.vlan': ('M', m_vlan),
}


# -- client: runs inside one build's Python environment ------------------------------

class Builder:
    def __init__(self, bess):
        self.bess = bess

    def mod(self, mclass, name, arg):
        self.bess.create_module(mclass, name, arg)

    def conn(self, a, b, ogate=0, igate=0):
        self.bess.connect_modules(a, b, ogate, igate)

    def cmd(self, module, command, arg_type, arg):
        self.bess.run_module_command(module, command, arg_type, arg)


def leaf_packets(bess, modules):
    """(packets, timestamp) summed over the leaf classes of `modules`' tasks."""
    total, stamp = 0, 0.0
    for c in bess.list_tcs().classes_status:
        tc = getattr(c, 'class')
        if tc.policy == 'leaf' and any(tc.name.startswith('!leaf_%s:' % m) for m in modules):
            st = bess.get_tc_stats(tc.name)
            total += st.packets
            stamp = max(stamp, st.timestamp)
    return total, stamp


def client(args):
    if os.environ.get('LIVE_PERF_PB2D'):  # master's pybess needs a newer protobuf_to_dict
        import importlib.util
        spec = importlib.util.spec_from_file_location('pybess.protobuf_to_dict',
                                                      os.environ['LIVE_PERF_PB2D'])
        mod = importlib.util.module_from_spec(spec)
        sys.modules['pybess.protobuf_to_dict'] = mod
        spec.loader.exec_module(mod)
    from pybess.bess import BESS
    bess = BESS()
    bess.connect(grpc_url='localhost:%d' % args.port)
    cpus = [int(c) for c in args.cpu.split(',')]
    out = {}
    for name in args.tests.split(','):
        try:
            bess.pause_all()
            bess.reset_all()
            bess.add_worker(0, cpus[0])
            if len(cpus) > 1:
                bess.add_worker(1, cpus[1])
            modules = TESTS[name][1](Builder(bess))
            bess.resume_all()
            time.sleep(args.warmup)
            p0, t0 = leaf_packets(bess, modules)
            time.sleep(args.duration)
            p1, t1 = leaf_packets(bess, modules)
            out[name] = (t1 - t0) * 1e9 / (p1 - p0) if p1 > p0 and t1 > t0 else None
        except Exception as e:  # recorded, not fatal: a test the build cannot run
            out[name] = 'error: %s' % str(e).splitlines()[0][:160]
    bess.pause_all()
    bess.reset_all()
    print(json.dumps(out))


# -- runner -------------------------------------------------------------------------

def build_spec(text):
    """'master', or 'current:<tree>' (its build/perf-release), or
    'name=<bessd>,<pythonpath>' for anything else."""
    if text == 'master':
        return {'name': 'master', 'bessd': str(MASTER / 'bin-v3/bessd'),
                'modules': str(MASTER / 'bin-v3/modules'), 'ld': '',
                'pythonpath': [str(MASTER_TOOLCHAIN), str(MASTER)],
                'env': {'LIVE_PERF_PB2D': str(ROOT / 'pybess/protobuf_to_dict.py')}}
    if text.startswith('current'):
        parts = text.split(':')
        tree = Path(parts[1]) if len(parts) > 1 else ROOT
        build = parts[2] if len(parts) > 2 else 'build/perf-release'
        gen = tree / build / 'protobuf/generated/python'
        return {'name': 'current' if len(parts) == 1 else tree.name + ('' if len(parts) < 3 else '/' + Path(build).name),
                'bessd': str(tree / build / 'core/bessd'), 'modules': '',
                'ld': str(DPDK_LIB), 'pythonpath': [str(gen / 'builtin_pb'), str(gen), str(tree)],
                'env': {'BESS_PROTOBUF_ROOT': str(gen)}}
    name, rest = text.split('=', 1)
    bessd, pythonpath = rest.split(',', 1)
    return {'name': name, 'bessd': bessd, 'modules': '', 'ld': str(DPDK_LIB),
            'pythonpath': pythonpath.split(':'), 'env': {}}


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run_round(spec, tests, args):
    port = free_port()
    pidfile = '/tmp/live_perf_%d.pid' % port
    cmd = ['sudo', '-n', 'env']
    if spec['ld']:
        cmd.append('LD_LIBRARY_PATH=' + spec['ld'])
    cmd += [spec['bessd'], '-f', '-p', str(port), '--skip_root_check', '-m', '0', '-i', pidfile,
            '-c', args.cpu.split(',')[-1]]
    if spec['modules']:
        cmd.append('--modules=' + spec['modules'])
    daemon = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        env = dict(os.environ, PYTHONPATH=':'.join(spec['pythonpath']), **spec['env'])
        for _ in range(60):
            with socket.socket() as s:
                if s.connect_ex(('127.0.0.1', port)) == 0:
                    break
            time.sleep(0.25)
        r = subprocess.run([sys.executable, __file__, '--client', '--port', str(port),
                            '--tests', ','.join(tests), '--cpu', args.cpu,
                            '--duration', str(args.duration), '--warmup', str(args.warmup)],
                           capture_output=True, text=True, env=env, timeout=1800)
        lines = [l for l in r.stdout.splitlines() if l.startswith('{')]
        if not lines:
            raise RuntimeError('%s: client failed: %s' % (spec['name'], r.stderr[-600:]))
        return json.loads(lines[-1])
    finally:
        subprocess.run(['sudo', '-n', 'pkill', '-f', 'bessd .* -p %d ' % port], check=False)
        daemon.wait(timeout=10)
        subprocess.run(['sudo', '-n', 'rm', '-f', pidfile], check=False)


def report(names, builds, data):
    """Medians per build, M tests also as the delta above M.base."""
    med = {}
    for b in builds:
        for t in names:
            vals = [v for v in data[b][t] if isinstance(v, (int, float))]
            med[(b, t)] = statistics.median(vals) if vals else None
    first, last = builds[0], builds[-1]
    print('\n%-18s %s %s' % ('test', ' '.join('%12s' % b for b in builds),
                             '%9s  %s' % ('change', 'pairs %s>%s' % (last, first))))
    for cls, title in (('F', 'framework (cross-cutting): ns/packet'),
                       ('M', 'modules (localised): ns/packet above M.base')):
        print('-- %s' % title)
        for t in names:
            if TESTS[t][0] != cls:
                continue
            row = []
            for b in builds:
                v = med[(b, t)]
                if cls == 'M' and t != 'M.base' and v is not None and med.get((b, 'M.base')):
                    v -= med[(b, 'M.base')]
                row.append(v)
            a, z = row[0], row[-1]
            change = '%+8.1f%%' % ((z / a - 1) * 100) if a and z is not None and a > 0 else '%9s' % 'n/a'
            pa = [x for x in data[first][t] if isinstance(x, (int, float))]
            pz = [x for x in data[last][t] if isinstance(x, (int, float))]
            worse = sum(1 for x, y in zip(pa, pz) if y > x)
            errors = {v for b in builds for v in data[b][t] if isinstance(v, str)}
            print('%-18s %s %s  %d/%d%s' % (t, ' '.join('%12s' % ('%.2f' % v if v is not None else 'n/a')
                                                         for v in row), change, worse,
                                          min(len(pa), len(pz)),
                                          ('  ' + '; '.join(sorted(errors))) if errors else ''))
    return med


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--client', action='store_true', help=argparse.SUPPRESS)
    p.add_argument('--port', type=int, help=argparse.SUPPRESS)
    p.add_argument('--tests', default=','.join(TESTS), help='comma-separated, or a class prefix (F, M)')
    p.add_argument('--build', action='append', default=[], help='master | current[:<tree>] | name=<bessd>,<pythonpath>')
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--cpu', default='2,4', help='worker 0 (and worker 1 for F.queue); the last also runs bessd\'s main thread')
    p.add_argument('--duration', type=float, default=1.0)
    p.add_argument('--warmup', type=float, default=0.3)
    p.add_argument('--save', help='write medians and raw values here')
    p.add_argument('--baseline', help='a saved run: compare the last build against it')
    p.add_argument('--threshold', type=float, default=5.0, help='percent slower that fails --baseline')
    args = p.parse_args()
    if args.client:
        client(args)
        return 0
    tests = [t for t in TESTS if any(t == s or t.startswith(s + '.') for s in args.tests.split(','))]
    if 'M.base' not in tests and any(t.startswith('M.') for t in tests):
        tests.insert(0, 'M.base')
    specs = [build_spec(b) for b in (args.build or ['current'])]
    names = [s['name'] for s in specs]
    data = {n: {t: [] for t in tests} for n in names}
    for r in range(args.rounds):
        for s in (specs if r % 2 == 0 else specs[::-1]):
            t0 = time.time()
            got = run_round(s, tests, args)
            for t in tests:
                data[s['name']][t].append(got.get(t))
            print('round %d %-10s %.0fs' % (r + 1, s['name'], time.time() - t0), file=sys.stderr)
    med = report(tests, names, data)
    if args.save:
        Path(args.save).write_text(json.dumps({
            'tests': tests, 'builds': names, 'raw': data, 'cpu': args.cpu, 'rounds': args.rounds,
            'duration': args.duration,
            'median': {b: {t: med[(b, t)] for t in tests} for b in names}}, indent=2))
    if args.baseline:
        base = json.loads(Path(args.baseline).read_text())
        ref = base['median'][base['builds'][-1]]
        last = names[-1]
        failed = []
        for t in tests:
            a, z = ref.get(t), med[(last, t)]
            if a and z and (z / a - 1) * 100 > args.threshold:
                failed.append('%s %+.1f%%' % (t, (z / a - 1) * 100))
        print('\nagainst %s: %s' % (args.baseline, ', '.join(failed) if failed else 'no test slower than %.1f%%' % args.threshold))
        return 1 if failed else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
