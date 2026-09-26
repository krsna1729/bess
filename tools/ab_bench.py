#!/usr/bin/env python3
# Copyright (c) 2026, Nefeli Networks, Inc.
# All rights reserved. (BSD 3-clause; see LICENSE.)
"""Paired A/B comparison of two Google Benchmark binaries, drift-cancelling.

Runs A and B in ABBA order (A B B A A B B A ...) so that slow drift (thermal,
frequency, background load) hits both sides equally, then compares them *per
adjacent pair* rather than as two independent averages:

  - per benchmark: median time of A and of B;
  - the paired ratio B/A of each ABBA pair (A and B runs that sat next to
    each other), its median and min..max;
  - how many pairs agree on the sign ("B faster in 7/8 pairs").

A difference is only called when the median paired ratio is outside the
noise band (default +/-3%) AND at least 3/4 of the pairs agree on the sign.

Usage:
  tools/ab_bench.py A_BINARY B_BINARY --filter REGEX [--rounds 8]
      [--min-time 0.2s] [--wrap "taskset -c 2"]
      [--noise 0.03] [-- extra benchmark args]

Before running, a pre-flight check samples every process for one second
and refuses to start if any process outside this tool uses more than 20% of
a CPU (a stray or runaway process skews even isolated runs through shared
power, thermal and cache budget). --allow-busy overrides it.

Both binaries get the same filter and arguments, unless --filter-b is given:
then B runs its own filter and --rename-b (a regex and a replacement, as
"PATTERN=>REPLACEMENT") maps B's benchmark names onto A's, so two
implementations inside one binary can be paired, e.g.
  tools/ab_bench.py BIN BIN --filter 'BM_Lookup/0/' --filter-b 'BM_Lookup/1/' \
      --rename-b 'BM_Lookup/1/=>BM_Lookup/0/'

--wrap is a command prefix that pins or isolates each run (default
"taskset -c 2"); pass a cgroup-isolation helper where one is available.
"""

import argparse
import os
import time
import collections
import json
import re
import shlex
import statistics
import subprocess
import sys


def busy_processes(threshold=0.2, window=1.0):
    """(pid, cpu_fraction, command) of processes using more than `threshold`
    of a CPU over `window` seconds, excluding this tool and its parents."""
    tick = os.sysconf('SC_CLK_TCK')

    def sample():
        out = {}
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open('/proc/%s/stat' % pid) as f:
                    fields = f.read().rsplit(')', 1)[1].split()
                out[int(pid)] = int(fields[11]) + int(fields[12])
            except (OSError, IndexError, ValueError):
                pass
        return out

    exclude = set()
    pid = os.getpid()
    while pid > 1:
        exclude.add(pid)
        try:
            with open('/proc/%d/stat' % pid) as f:
                pid = int(f.read().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    before = sample()
    time.sleep(window)
    after = sample()
    busy = []
    for pid, ticks in after.items():
        if pid in exclude or pid not in before:
            continue
        frac = (ticks - before[pid]) / tick / window
        if frac > threshold:
            try:
                with open('/proc/%d/cmdline' % pid, 'rb') as f:
                    cmd = f.read().replace(b'\0', b' ').decode(errors='replace')
            except OSError:
                cmd = '?'
            busy.append((pid, frac, cmd.strip()[:100]))
    return busy


def run(binary, args, wrap):
    cmd = shlex.split(wrap) + [binary] + args + ['--benchmark_format=json']
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit('benchmark failed: %s\n%s' % (' '.join(cmd), out.stderr[-2000:]))
    # Wrappers may print their own lines; the JSON document starts at '{'.
    text = out.stdout[out.stdout.index('{'):]
    data = json.loads(text)
    return {b['name']: b['real_time'] for b in data['benchmarks']
            if b.get('run_type', 'iteration') == 'iteration' and 'real_time' in b}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('a')
    p.add_argument('b')
    p.add_argument('--filter', required=True)
    p.add_argument('--filter-b')
    p.add_argument('--rename-b', help='PATTERN=>REPLACEMENT for B names')
    p.add_argument('--rounds', type=int, default=8,
                   help='runs per side (even; ABBA pairs = rounds)')
    p.add_argument('--min-time', default='0.2s')
    p.add_argument('--wrap', default='taskset -c 2')
    p.add_argument('--noise', type=float, default=0.03)
    p.add_argument('--allow-busy', action='store_true',
                   help='skip the idle-machine pre-flight check')
    p.add_argument('extra', nargs='*')
    o = p.parse_args()

    if not o.allow_busy:
        busy = busy_processes()
        if busy:
            for pid, frac, cmd in busy:
                print('busy: pid %d at %.0f%% CPU: %s' % (pid, 100 * frac, cmd),
                      file=sys.stderr)
            sys.exit('refusing to benchmark on a busy machine '
                     '(stop those processes, or pass --allow-busy)')

    common = ['--benchmark_min_time=' + o.min_time] + o.extra
    args = {'a': ['--benchmark_filter=' + o.filter] + common,
            'b': ['--benchmark_filter=' + (o.filter_b or o.filter)] + common}
    rename = None
    if o.rename_b:
        pattern, replacement = o.rename_b.split('=>', 1)
        rename = (re.compile(pattern), replacement)
    order = []
    for i in range(o.rounds // 2):
        order += ['a', 'b', 'b', 'a']
    results = {'a': [], 'b': []}
    for step, side in enumerate(order):
        r = run(o.a if side == 'a' else o.b, args[side], o.wrap)
        if side == 'b' and rename:
            r = {rename[0].sub(rename[1], k): v for k, v in r.items()}
        results[side].append(r)
        print('.', end='', flush=True, file=sys.stderr)
    print(file=sys.stderr)

    # ABBA block k holds A[2k], B[2k], B[2k+1], A[2k+1]: pair each A with
    # the B next to it.
    names = [n for n in results['a'][0] if all(n in r for r in results['a'] + results['b'])]
    print('| benchmark | A median | B median | paired B/A median (min..max) | B faster in | verdict |')
    print('|---|---|---|---|---|---|')
    for n in names:
        a = [r[n] for r in results['a']]
        b = [r[n] for r in results['b']]
        ratios = [b[i] / a[i] for i in range(len(a))]
        med = statistics.median(ratios)
        faster = sum(r < 1 for r in ratios)
        k = len(ratios)
        agree = max(faster, k - faster)
        if abs(med - 1) <= o.noise or agree < 0.75 * k:
            verdict = 'no clear difference'
        else:
            verdict = 'B %+.1f%%' % (100 * (med - 1))
        print('| %s | %.4g | %.4g | %.3f (%.3f..%.3f) | %d/%d | %s |' % (
            n, statistics.median(a), statistics.median(b), med, min(ratios),
            max(ratios), faster, k, verdict))


if __name__ == '__main__':
    main()
