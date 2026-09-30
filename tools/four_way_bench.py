#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Four-way drift-cancelling benchmark comparator for BESS.

Runs 4 configurations in palindromic order (A B C D D C B A ...) under
omarchy-benchmark isolation, collects per-round JSON metrics, and computes
paired/median statistics for a 4-point comparison table:
  1: Master (native)
  2: Master (x86-64-v3)
  3: Current (x86-64-v3)
  4: Current (native)
"""

import argparse
import collections
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path


def busy_processes(threshold=0.2, window=1.0):
    def sample():
        stats = {}
        try:
            out = subprocess.check_output(
                ['ps', '-eo', 'pid,utime,stime,comm'], text=True
            )
        except subprocess.SubprocessError:
            return stats
        for line in out.strip().splitlines()[1:]:
            parts = line.split(None, 3)
            if len(parts) == 4:
                pid, ut, st, comm = parts
                try:
                    stats[int(pid)] = (int(ut) + int(st), comm)
                except ValueError:
                    continue
        return stats

    s1 = sample()
    time.sleep(window)
    s2 = sample()
    busy = []
    my_pid = os.getpid()
    for pid, (ticks2, comm) in s2.items():
        if pid == my_pid:
            continue
        if pid in s1:
            ticks1, _ = s1[pid]
            clk_tck = os.sysconf(os.sysconf_names['SC_CLK_TCK'])
            cpu_frac = (ticks2 - ticks1) / (clk_tck * window)
            if cpu_frac >= threshold:
                busy.append((pid, cpu_frac, comm))
    return busy


def run_single(name, binary, args, wrap, env=None):
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.json', delete=False) as tmp:
        tmp_path = Path(tmp.name)
    cmd = shlex.split(wrap) + [binary] + args + [f'--benchmark_out={tmp_path}', '--benchmark_out_format=json']
    env_copy = os.environ.copy()
    if env:
        env_copy.update(env)
    
    try:
        res = subprocess.run(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=env_copy, check=True
        )
    except subprocess.CalledProcessError as e:
        print(f"Error running {name} ({cmd}): rc={e.returncode}", file=sys.stderr)
        print(f"Stdout:\n{e.stdout}", file=sys.stderr)
        print(f"Stderr:\n{e.stderr}", file=sys.stderr)
        tmp_path.unlink(missing_ok=True)
        return {}

    try:
        with open(tmp_path) as f:
            payload = json.load(f)
    except Exception as e:
        print(f"Error reading JSON output for {name}: {e}", file=sys.stderr)
        payload = {}
    finally:
        tmp_path.unlink(missing_ok=True)

    results = {}
    for b in payload.get('benchmarks', []):
        if b.get('run_type', 'iteration') == 'iteration' and 'real_time' in b:
            bench_name = b['name']
            results[bench_name] = {
                'real_time': b['real_time'],
                'cpu_time': b.get('cpu_time', b['real_time']),
                'items_per_second': b.get('items_per_second', None),
                'time_unit': b.get('time_unit', 'ns')
            }
    return results


def run_current_only(args):
    v3_dir = Path('/home/krsna1729/Projects/bess/build/perf-release/core')
    native_dir = Path('/home/krsna1729/Projects/bess/build/perf-release-native/core')
    # These are purpose-built runners, not Google Benchmark executables.
    special_binaries = {
        'grace_period_bench': 'standalone dataplane/QSBR report',
        'occupancy_bench': 'standalone DPDK occupancy report',
        'ingress_bench': 'standalone dataplane ingress report',
        'update_scale_bench': 'standalone update scale report',
    }
    common_bins = {
        'traffic_class_bench',
        'utils_cuckoo_map_bench',
        'utils_checksum_bench',
        'utils_copy_bench',
        'modules_url_filter_bench',
    }
    binaries = sorted(
        path.name for path in v3_dir.iterdir()
        if path.is_file() and path.name.endswith('_bench')
        and path.name not in common_bins and path.name not in special_binaries
    )
    missing = {
        name: 'current-native executable is not built'
        for name in binaries if not (native_dir / name).is_file()
    }
    binaries = [name for name in binaries if name not in missing]
    if missing:
        for name, reason in missing.items():
            print(f"SKIP {name}: {reason}", file=sys.stderr)

    env = {'LD_LIBRARY_PATH': '/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib'}
    targets = {
        'current_v3': {
            'label': 'Current (x86-64-v3)',
            'binary_dir': v3_dir,
            'env': env,
        },
        'current_nat': {
            'label': 'Current (native)',
            'binary_dir': native_dir,
            'env': env,
        },
    }
    wrap = f"omarchy-benchmark --isolate --cpu {args.cpu} --"
    bench_args = []
    if args.filter:
        bench_args.append(f"--benchmark_filter={args.filter}")
    if args.min_time:
        bench_args.append(f"--benchmark_min_time={args.min_time}")
    bench_args.extend(args.extra)

    seq = ['current_v3', 'current_nat', 'current_nat', 'current_v3']
    data = {key: collections.defaultdict(list) for key in targets}
    failures = collections.defaultdict(dict)
    total_runs = len(seq) * args.rounds
    print(f"Running {len(binaries)} current-only Google Benchmark binaries, {total_runs} palindromic runs each on CPU {args.cpu}...")
    print(f"Run custom binaries separately: {', '.join(special_binaries)}")

    for binary_name in binaries:
        failed = False
        run_idx = 0
        for _ in range(args.rounds):
            for key in seq:
                if failed:
                    break
                run_idx += 1
                binary = targets[key]['binary_dir'] / binary_name
                try:
                    result = run_single(
                        f"{targets[key]['label']} {binary_name}",
                        str(binary), bench_args, wrap, targets[key]['env'])
                except SystemExit:
                    failures[binary_name][key] = 'benchmark executable failed'
                    failed = True
                    continue
                print(f"[{run_idx}/{total_runs}] {targets[key]['label']} {binary_name}: {len(result)} cases")
                if not result:
                    failures[binary_name][key] = 'no iteration results'
                    failed = True
                    continue
                for case, metrics in result.items():
                    data[key][f"{binary_name}/{case}"].append(metrics)

    print("\nCURRENT-ONLY MICROBENCHMARKS (latency; lower is better)")
    print("| Benchmark | Current v3 | Current native | Native vs v3 |")
    print("|---|---:|---:|---:|")
    all_cases = sorted(set(data['current_v3']) | set(data['current_nat']))
    for case in all_cases:
        v3 = data['current_v3'].get(case, [])
        native = data['current_nat'].get(case, [])
        v3_med = statistics.median(m['real_time'] for m in v3) if v3 else None
        native_med = statistics.median(m['real_time'] for m in native) if native else None
        unit = (v3 or native)[0]['time_unit']

        def fmt(value):
            return f'{value:.3f} {unit}' if value is not None else 'n/a'

        if v3_med is None or native_med is None or v3_med <= 0:
            delta = 'n/a'
        else:
            delta = f'{(native_med / v3_med - 1.0) * 100:+.1f}%'
        print(f"| {case} | {fmt(v3_med)} | {fmt(native_med)} | {delta} |")

    if failures:
        print("\nFailed current-only benchmark binaries:")
        for binary_name, per_variant in failures.items():
            print(f"- {binary_name}: {per_variant}")

    if args.output:
        with open(args.output, 'w') as f:
            json.dump({
                'special_binaries': special_binaries,
                'binaries': binaries,
                'data': data,
                'missing_current_native': missing,
                'failures': failures,
                'metric': 'Google Benchmark real_time',
            }, f, indent=2)
        print(f"\nRaw results saved to {args.output}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--suite', default='cuckoo', choices=['cuckoo', 'tc', 'checksum', 'copy', 'url_filter', 'current-only'],
                   help='Benchmark suite, or current-only for all additional current binaries')
    p.add_argument('--filter', default='', help='Google Benchmark regex filter')
    p.add_argument('--rounds', type=int, default=4, help='Number of palindromic rounds (default: 4, yielding 8 runs per target)')
    p.add_argument('--min-time', default='0.25s', help='--benchmark_min_time per run')
    p.add_argument('--cpu', default='2', help='Pinned CPU core (default: 2)')
    p.add_argument('--allow-busy', action='store_true', help='Skip pre-flight CPU check')
    p.add_argument('--output', default='', help='Path to save raw JSON results')
    p.add_argument('--extra', nargs=argparse.REMAINDER, default=[], help='Extra args to benchmark')
    args = p.parse_args()

    if not args.allow_busy:
        busy = busy_processes()
        if busy:
            print("System busy before benchmark:", file=sys.stderr)
            for pid, frac, comm in busy:
                print(f"  PID {pid} ({comm}): {frac*100:.1f}% CPU", file=sys.stderr)
            sys.exit(1)
    if args.suite == 'current-only':
        run_current_only(args)
        return

    suite_binaries = {
        'cuckoo': {
            'm_bin': 'cuckoo_map_bench',
            'c_bin': 'utils_cuckoo_map_bench',
            'filter_def': ''
        },
        'tc': {
            'm_bin': 'traffic_class_bench',
            'c_bin': 'traffic_class_bench',
            'filter_def': ''
        },
        'checksum': {
            'm_bin': 'checksum_bench',
            'c_bin': 'utils_checksum_bench',
            'filter_def': ''
        },
        'copy': {
            'm_bin': 'copy_bench',
            'c_bin': 'utils_copy_bench',
            'filter_def': ''
        },
        'url_filter': {
            'm_bin': 'url_filter_bench',
            'c_bin': 'modules_url_filter_bench',
            'filter_def': ''
        }
    }

    cfg = suite_binaries[args.suite]
    wrap = f"omarchy-benchmark --isolate --cpu {args.cpu} --"
    targets = {
        'master_nat': {
            'label': 'Master (native)',
            'binary': f"/var/tmp/bess-clawback-base-20260930/bin-native/{cfg['m_bin']}",
            'env': {}
        },
        'master_v3': {
            'label': 'Master (x86-64-v3)',
            'binary': f"/var/tmp/bess-clawback-base-20260930/bin-v3/{cfg['m_bin']}",
            'env': {}
        },
        'current_v3': {
            'label': 'Current (x86-64-v3)',
            'binary': f"/home/krsna1729/Projects/bess/build/perf-release/core/{cfg['c_bin']}",
            'env': {'LD_LIBRARY_PATH': '/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib'}
        },
        'current_nat': {
            'label': 'Current (native)',
            'binary': f"/home/krsna1729/Projects/bess/build/perf-release-native/core/{cfg['c_bin']}",
            'env': {'LD_LIBRARY_PATH': '/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib'}
        }
    }

    # Order of palindromic runs per cycle: A, B, C, D, D, C, B, A
    seq = ['master_nat', 'master_v3', 'current_v3', 'current_nat',
           'current_nat', 'current_v3', 'master_v3', 'master_nat']

    bench_args = []
    if args.filter:
        bench_args.append(f"--benchmark_filter={args.filter}")
    elif cfg.get('filter_def'):
        bench_args.append(f"--benchmark_filter={cfg['filter_def']}")
    if args.min_time:
        bench_args.append(f"--benchmark_min_time={args.min_time}")
    bench_args.extend(args.extra)

    data = {key: collections.defaultdict(list) for key in targets}

    total_runs = len(seq) * args.rounds
    run_idx = 0
    print(f"Running {args.rounds} palindromic rounds ({total_runs} total runs) on CPU {args.cpu}...")

    for r in range(args.rounds):
        for key in seq:
            run_idx += 1
            t0 = time.time()
            res = run_single(targets[key]['label'], targets[key]['binary'], bench_args, wrap, targets[key]['env'])
            dt = time.time() - t0
            print(f"[{run_idx}/{total_runs}] {targets[key]['label']}: {len(res)} cases ({dt:.2f}s)")
            for bname, metrics in res.items():
                data[key][bname].append(metrics['real_time'])

    # Print markdown table
    all_cases = sorted(list(data['master_nat'].keys()))
    print("\n" + "=" * 100)
    print("FOUR-POINT BENCHMARK COMPARISON RESULTS")
    print("=" * 100)
    
    headers = [
        "Benchmark Case",
        "Master (nat)",
        "Master (v3)",
        "Current (v3)",
        "Current (nat)",
        "Curr-v3 / M-v3",
        "Curr-nat / M-nat",
        "Curr-v3 / M-nat"
    ]
    print(f"| {' | '.join(headers)} |")
    print(f"|{'-' * 35}|{'-' * 14}|{'-' * 14}|{'-' * 14}|{'-' * 14}|{'-' * 16}|{'-' * 17}|{'-' * 16}|")

    for case in all_cases:
        mn_vals = data['master_nat'][case]
        mv_vals = data['master_v3'][case]
        cv_vals = data['current_v3'][case]
        cn_vals = data['current_nat'][case]

        if not (mn_vals and mv_vals and cv_vals and cn_vals):
            continue

        mn_med = statistics.median(mn_vals)
        mv_med = statistics.median(mv_vals)
        cv_med = statistics.median(cv_vals)
        cn_med = statistics.median(cn_vals)

        # Ratios
        cv_mv_ratio = cv_med / mv_med
        cn_mn_ratio = cn_med / mn_med
        cv_mn_ratio = cv_med / mn_med

        cv_mv_pct = (cv_mv_ratio - 1.0) * 100.0
        cn_mn_pct = (cn_mn_ratio - 1.0) * 100.0
        cv_mn_pct = (cv_mn_ratio - 1.0) * 100.0

        short_case = case.replace("CuckooMapFixture/", "").replace("TCWeightedFair/", "").replace("TCRoundRobin/", "")

        print(f"| {short_case:<33} | {mn_med:12.3f} | {mv_med:12.3f} | {cv_med:12.3f} | {cn_med:12.3f} | {cv_mv_pct:+6.1f}% ({cv_mv_ratio:.3f}) | {cn_mn_pct:+6.1f}% ({cn_mn_ratio:.3f}) | {cv_mn_pct:+6.1f}% ({cv_mn_ratio:.3f}) |")

    if args.output:
        with open(args.output, 'w') as f:
            json.dump({'all_cases': all_cases, 'data': data}, f, indent=2)
        print(f"\nRaw results saved to {args.output}")


if __name__ == '__main__':
    main()
