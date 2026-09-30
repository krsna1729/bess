#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Four-way live dataplane throughput comparison for port-free BESS configs.

Runs every samples/perftest .bess configuration that needs no physical PMD
port, using software Source/Sink paths and isolated worker CPUs:
  1: Master (native)
  2: Master (x86-64-v3)
  3: Current (x86-64-v3)
  4: Current (native)
"""

import argparse
import collections
import importlib.util
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import time

CURRENT_ROOT = Path('/home/krsna1729/Projects/bess')
MASTER_ROOT = Path('/var/tmp/bess-clawback-base-20260930')
PHYSICAL_PORT_CONFIGS = {
    'perftest/flowgen',
    'perftest/phy_forward',
    'perftest/pktgen',
}


def discover_pipelines():
    available = {}
    for root in (CURRENT_ROOT, MASTER_ROOT):
        for section in ('samples', 'perftest'):
            config_dir = root / 'bessctl' / 'conf' / section
            for path in sorted(config_dir.glob('*.bess')):
                name = f'{section}/{path.stem}'
                available.setdefault(name, path)

    pipelines = []
    config_sources = {}
    for name, path in sorted(available.items()):
        if name in PHYSICAL_PORT_CONFIGS:
            continue
        env_vars = {}
        if name == 'perftest/loopback_vport':
            env_vars = {
                'BESS_CORE_START': '2',
                'BESS_CORE_END': '5',
                'BESS_CORE_STEP': '2',
                'BESS_INTERVAL': '1',
            }
        pipelines.append((name, str(path), env_vars))
        config_sources[name] = str(path)

    excluded = {
        name: 'requires a physical PMDPort'
        for name in sorted(PHYSICAL_PORT_CONFIGS & available.keys())
    }
    return pipelines, excluded, config_sources


SUITE_PIPELINES, EXCLUDED_PIPELINES, CONFIG_SOURCES = discover_pipelines()

VARIANTS = {
    'master_nat': {
        'label': 'Master (native)',
        'bessd': '/var/tmp/bess-clawback-base-20260930/bin-native/bessd',
        'modules': '/var/tmp/bess-clawback-base-20260930/bin-native/modules',
        'ld_path': '',
        'is_master': True
    },
    'master_v3': {
        'label': 'Master (x86-64-v3)',
        'bessd': '/var/tmp/bess-clawback-base-20260930/bin-v3/bessd',
        'modules': '/var/tmp/bess-clawback-base-20260930/bin-v3/modules',
        'ld_path': '',
        'is_master': True
    },
    'current_v3': {
        'label': 'Current (x86-64-v3)',
        'bessd': '/home/krsna1729/Projects/bess/build/perf-release/core/bessd',
        'modules': '',
        'ld_path': '/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib',
        'is_master': False
    },
    'current_nat': {
        'label': 'Current (native)',
        'bessd': '/home/krsna1729/Projects/bess/build/perf-release-native/core/bessd',
        'modules': '',
        'ld_path': '/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib',
        'is_master': False
    }
}


def allocate_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(('0.0.0.0', 0))
        return sock.getsockname()[1]


def remove_pidfile(path):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def stop_bessd(proc, pidfile):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    remove_pidfile(pidfile)


def run_pipeline_worker(variant_key, pipelines, cpu='2,4', duration=1.0):
    info = VARIANTS[variant_key]
    worker_cpus = [int(core) for core in cpu.split(',')]
    port = allocate_tcp_port()
    pidfile = f"/tmp/bessd_bench_{os.getpid()}_{port}.pid"

    # Start bessd in background
    cmd = ['sudo']
    if info['ld_path']:
        cmd.extend(['env', f"LD_LIBRARY_PATH={info['ld_path']}"])
    cmd.extend([
        info['bessd'], '-f', '-c', str(worker_cpus[0]), '-p', str(port),
        '--skip_root_check', '-m', '0', '-i', pidfile
    ])
    if info['modules']:
        cmd.append(f"--modules={info['modules']}")

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ready = False
    for _ in range(40):
        try:
            import grpc
            ch = grpc.insecure_channel(f'localhost:{port}')
            grpc.channel_ready_future(ch).result(timeout=0.3)
            ch.close()
            ready = True
            break
        except Exception:
            time.sleep(0.25)
    if not ready:
        stop_bessd(proc, pidfile)
        raise RuntimeError(f"bessd {variant_key} did not become ready on port {port}")

    results = {}
    skipped = {}
    try:
        # Run measuring client in a subprocess to cleanly isolate python imports & protobuf schemas
        client_code = f"""
import sys, os, time, json, importlib.util

variant = "{variant_key}"
is_master = {info['is_master']}

if is_master:
    # Master tree imports
    spec = importlib.util.spec_from_file_location('pybess.protobuf_to_dict', '/home/krsna1729/Projects/bess/pybess/protobuf_to_dict.py')
    mod = importlib.util.module_from_spec(spec)
    sys.modules['pybess.protobuf_to_dict'] = mod
    spec.loader.exec_module(mod)
    sys.path.insert(0, '/var/tmp/master-bench-toolchain')
    sys.path.insert(1, '/var/tmp/bess-clawback-base-20260930')
    sys.path.insert(2, '/var/tmp/bess-clawback-base-20260930/bessctl')
    import cli as cli_mod
    import commands
    from pybess.bess import BESS
    bess_dir = '/var/tmp/bess-clawback-base-20260930/bessctl'
else:
    # Current tree imports
    sys.path.insert(0, '/home/krsna1729/Projects/bess/build/perf-release/protobuf/generated/python/builtin_pb')
    sys.path.insert(1, '/home/krsna1729/Projects/bess/build/perf-release/protobuf/generated/python')
    sys.path.insert(2, '/home/krsna1729/Projects/bess')
    sys.path.insert(3, '/home/krsna1729/Projects/bess/bessctl')
    import bessctl.cli as cli_mod
    import bessctl.commands as commands
    from pybess.bess import BESS
    bess_dir = '/home/krsna1729/Projects/bess/bessctl'

BESS.track_module = lambda self, *a, **k: None

bess = BESS()
bess.connect(grpc_url='localhost:{port}')
cli = cli_mod.CLI(commands.cmdlist)
cli.bess = bess
cli.this_dir = bess_dir

worker_cpus = {worker_cpus!r}
original_add_worker = bess.add_worker
def remap_add_worker(wid, core, scheduler=None):
    core = int(core)
    if 0 <= core < len(worker_cpus):
        core = worker_cpus[core]
    return original_add_worker(wid, core, scheduler)
bess.add_worker = remap_add_worker

pipelines = {pipelines}
res = {{}}
skipped = {{}}
for name, path, env_vars in pipelines:
    workers_before = set(w.wid for w in bess.list_workers().workers_status)
    try:
        bess.reset_all()
        commands._run_file(cli, path, env_vars)
        time.sleep(0.3)
        bess.resume_all()

        def sink_snapshot():
            snapshot = {{}}
            for module in bess.list_modules().modules:
                if module.mclass.rsplit('::', 1)[-1].lower() != 'sink':
                    continue
                info = bess.get_module_info(module.name)
                for igate in info.igates:
                    snapshot[(module.name, igate.igate)] = (
                        igate.pkts, igate.timestamp)
            return snapshot

        old = sink_snapshot()
        if not old:
            skipped[name] = 'no Sink input counters (setup-only config)'
            continue
        time.sleep({duration})
        new = sink_snapshot()
        pps = 0.0
        for key, (old_packets, old_timestamp) in old.items():
            if key in new:
                new_packets, new_timestamp = new[key]
                dt = new_timestamp - old_timestamp
                if dt > 0:
                    pps += (new_packets - old_packets) / dt
        res[name] = pps / 1e6
    except Exception as e:
        skipped[name] = str(e)
    finally:
        try:
            bess.pause_all()
            bess.reset_all()
            for worker in bess.list_workers().workers_status:
                if worker.wid not in workers_before:
                    bess.destroy_worker(worker.wid)
        except Exception:
            skipped[name] = 'worker cleanup failed'

print(json.dumps({{'results': res, 'skipped': skipped}}))
"""
        # Keep RPC client under the isolation wrapper; it sleeps during packet sampling.
        client_res = subprocess.run(
            ['python3', '-c', client_code],
            capture_output=True, text=True, timeout=120
        )
        report_found = False
        if client_res.returncode == 0 and client_res.stdout.strip():
            for line in reversed(client_res.stdout.strip().splitlines()):
                line = line.strip()
                if line.startswith('{') and line.endswith('}'):
                    report = json.loads(line)
                    report_found = True
                    results = report['results']
                    skipped = report['skipped']
                    for name, error in skipped.items():
                        print(f"  SKIP {variant_key}/{name}: {error}", file=sys.stderr)
                    break
            if not report_found:
                skipped['_client'] = 'no JSON report found'
        else:
            skipped['_client'] = client_res.stderr or 'client returned no JSON'
            print(f"Error in client for {variant_key}: stderr={client_res.stderr}", file=sys.stderr)
    finally:
        stop_bessd(proc, pidfile)

    return results, skipped


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rounds', type=int, default=3, help='Number of palindromic rounds (default: 3)')
    p.add_argument('--cpu', default='2,4',
                   help='Isolated worker CPUs (default: 2,4)')
    p.add_argument('--duration', type=float, default=1.0, help='Measurement window in seconds (default: 1.0)')
    p.add_argument('--output', default='/tmp/four_way_pipelines.json', help='Output JSON path')
    args = p.parse_args()

    seq = ['master_nat', 'master_v3', 'current_v3', 'current_nat',
           'current_nat', 'current_v3', 'master_v3', 'master_nat']

    total_runs = len(seq) * args.rounds

    print(f"Executing {args.rounds} palindromic rounds ({total_runs} daemon runs) across {len(SUITE_PIPELINES)} port-free configs on CPUs {args.cpu}...")
    print(f"Excluded physical-port configs: {', '.join(EXCLUDED_PIPELINES)}")

    data = {k: collections.defaultdict(list) for k in VARIANTS}
    skipped_data = {k: collections.defaultdict(list) for k in VARIANTS}

    run_idx = 0
    for r in range(args.rounds):
        for key in seq:
            run_idx += 1
            t0 = time.time()
            m, skipped = run_pipeline_worker(
                key, SUITE_PIPELINES, cpu=args.cpu, duration=args.duration)
            dt = time.time() - t0
            print(f"[{run_idx}/{total_runs}] {VARIANTS[key]['label']:<20}: {len(m)} configs measured, {len(skipped)} skipped ({dt:.2f}s)")
            for name, mpps in m.items():
                data[key][name].append(mpps)
            for name, error in skipped.items():
                skipped_data[key][name].append(error)

    print("\n" + "=" * 150)
    print("FOUR-POINT LIVE DATAPLANE THROUGHPUT TO SINKS (Mpps)")
    print("=" * 150)

    headers = [
        "Config",
        "Master (nat)",
        "Master (v3)",
        "Current (v3)",
        "Current (nat)",
        "Curr-v3 / M-v3",
        "Curr-nat / M-nat",
        "Curr-v3 / M-nat"
    ]
    print(f"| {' | '.join(headers)} |")
    print(f"|{'-' * 28}|{'-' * 14}|{'-' * 14}|{'-' * 14}|{'-' * 14}|{'-' * 16}|{'-' * 17}|{'-' * 16}|")

    all_names = [p[0] for p in SUITE_PIPELINES]
    for name in all_names:
        medians = {
            key: statistics.median(data[key][name]) if data[key][name] else None
            for key in VARIANTS
        }

        def fmt(value):
            return f'{value:.3f}' if value is not None else 'n/a'

        def delta(current, baseline):
            if current is None or baseline is None or baseline <= 0:
                return 'n/a'
            return f'{(current / baseline - 1.0) * 100:+.1f}%'

        print(
            f"| {name:<26} | {fmt(medians['master_nat']):>12} "
            f"| {fmt(medians['master_v3']):>12} | {fmt(medians['current_v3']):>12} "
            f"| {fmt(medians['current_nat']):>12} "
            f"| {delta(medians['current_v3'], medians['master_v3']):>16} "
            f"| {delta(medians['current_nat'], medians['master_nat']):>17} "
            f"| {delta(medians['current_v3'], medians['master_nat']):>16} |")

    print("\nSkipped or unmeasurable configs:")
    for name in all_names:
        details = []
        for key, variant in VARIANTS.items():
            reasons = sorted(set(skipped_data[key].get(name, [])))
            if reasons:
                details.append(f"{variant['label']}: {'; '.join(reasons)}")
        if details:
            print(f"- {name}: {' | '.join(details)}")

    if args.output:
        with open(args.output, 'w') as f:
            json.dump({
                'pipelines': all_names,
                'data': data,
                'skipped': skipped_data,
                'excluded_physical_port_configs': EXCLUDED_PIPELINES,
                'config_sources': CONFIG_SOURCES,
                'metric': 'sum of Sink input packet-counter deltas per second (Mpps)',
                'worker_cpus': args.cpu,
                'rounds': args.rounds,
                'duration_seconds': args.duration,
            }, f, indent=2)
        print(f"\nSaved raw results to {args.output}")


if __name__ == '__main__':
    main()
