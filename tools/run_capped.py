#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run one test under its declared memory bound and fail it above that.

Every meson test declares the most memory it may use, in MiB, as
BESS_TEST_MEM_MB in its environment (tests/memory/meson.build;
docs/testing.md#memory). Meson runs each test through this script (the default
test setup's exe_wrapper in meson.build), which:

  * runs the test in a cgroup of its own where it can: a transient
    `systemd-run --user --scope` with MemoryMax set to the kill limit
    (KILL_FACTOR times the declaration) and no swap, so a runaway test is
    stopped by the kernel inside its own cgroup instead of filling the
    session's;
  * records the peak of the whole process tree: the larger of the scope's
    memory.peak, less the page cache still charged to it at the end, and
    wait4()'s ru_maxrss (the largest single process of the tree). The page
    cache, because the cgroup is charged for files only the first time
    anyone reads them, so it is not the same from one run to the next, and
    the kernel takes it back at the limit before it kills; shared memory
    (DPDK's --no-huge heap is a memfd) is not page cache and stays counted.
    ru_maxrss, because the cgroup is not charged for pages another cgroup
    read first (the test binary's and its libraries'), which ru_maxrss
    counts; it sees one process at a time. With no user manager to make a
    scope, ru_maxrss alone;
  * adds to the cgroup's peak the peaks that tests report for the cgroups
    their children escape into (BESS_TEST_MEM_REPORT below: a live test's
    bessd started under a `systemd-run --scope` of its own);
  * fails the test when that peak is above the declaration:
    "declared X MiB, peak Y MiB".

Not RLIMIT_AS: DPDK reserves virtual address space far beyond what it touches
(the EAL's memseg lists reserve tens of GiB of VA at init, backed by
nothing), so an address-space limit fails EAL init in every test that starts
it while bounding nothing that is resident. The cgroup limit and the peak are
both resident (charged) memory, which is what fills a machine.

The report protocol: the wrapper sets BESS_TEST_MEM_REPORT to a file. A test
that starts a process in a cgroup outside its own appends one line per such
cgroup, "<label> <peak bytes>", before that cgroup goes away; the wrapper adds
those to its own peak (a sum of peaks: an upper bound on what was resident at
once). The self-test reports the largest of the scopes it makes the same way.

Usage (meson runs it; by hand, set the declaration):
  BESS_TEST_MEM_MB=64 tools/run_capped.py [--no-scope] CMD [ARGS...]
  tools/run_capped.py --self-test HOG

Environment:
  BESS_TEST_MEM_MB       the declaration, MiB (required)
  BESS_TEST_MEM_MEASURE  1: calibration; report the peak, never fail on it,
                         kill limit MEASURE_LIMIT_MB
  BESS_TEST_MEM_NO_SCOPE 1: as --no-scope (measure with wait4 only)
  BESS_TEST_MEM_SLICE    a systemd slice for the scopes: a whole `meson test`'s
                         peak is then that slice's memory.peak (a live test's
                         own bessd scope goes there too)
  BESS_TEST_MEM_REQUIRE_SCOPE 1: the self-test fails when it cannot make a
                         scope (CI, which starts a user manager for it)
"""

import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import uuid

MIB = 1 << 20
# The kernel's limit is above the declaration, so a test that overshoots it by
# less than half runs to the end and fails with its measured peak; one that
# runs away is stopped at the limit. At least 32 MiB above: a small test's
# declaration is mostly its binary's pages.
KILL_FACTOR = 1.5
KILL_MIN_HEADROOM_MB = 32
# The kill limit in measure mode: a runaway calibration run stops there.
MEASURE_LIMIT_MB = 3000

# Runs inside the scope: notes that the scope exists (so the wrapper can tell
# "systemd-run could not make it" from "the test failed"), runs the test as a
# child, then reads the scope's counters while it still exists (a scope goes
# away with its last process) and exits with the test's status.
INNER = r'''
out=$1; shift
cg=$(cat /proc/self/cgroup); cg=/sys/fs/cgroup${cg#0::}
echo started > "$out"
"$@"
rc=$?
{
  echo "peak $(cat "$cg/memory.peak" 2>/dev/null || echo -1)"
  echo "oom_kill $(sed -n 's/^oom_kill //p' "$cg/memory.events" 2>/dev/null)"
  echo "file $(sed -n 's/^file //p' "$cg/memory.stat" 2>/dev/null)"
  echo "shmem $(sed -n 's/^shmem //p' "$cg/memory.stat" 2>/dev/null)"
} >> "$out"
exit $rc
'''


def kill_limit_mb(declared_mb, measure):
    if measure:
        return MEASURE_LIMIT_MB
    return max(int(declared_mb * KILL_FACTOR + 0.5), declared_mb + KILL_MIN_HEADROOM_MB)


def read_lines(path):
    try:
        with open(path, encoding='utf-8') as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def spawn(cmd, env):
    """(status, ru_maxrss bytes): the child's wait4, which takes in every
    descendant it reaped; a signal N comes back as 128+N, as a shell says it.
    posix_spawn, not fork: a forked child's high-water mark would start at
    this interpreter's resident size."""
    pid = os.posix_spawnp(cmd[0], cmd, env)
    _, raw, usage = os.wait4(pid, 0)
    code = os.waitstatus_to_exitcode(raw)
    return (code if code >= 0 else 128 - code), usage.ru_maxrss * 1024


def run_in_scope(cmd, env, limit_mb):
    """(status, memory.peak bytes or -1, page cache bytes at the end, ru_maxrss
    bytes, oom kills), or None when no scope could be made."""
    if not shutil.which('systemd-run'):
        return None
    unit = f'bess-test-{os.getpid()}-{uuid.uuid4().hex[:8]}'
    with tempfile.TemporaryDirectory(prefix='run_capped.') as tmp:
        out = os.path.join(tmp, 'scope')  # the inner script creates it: the proof
        # systemd-run --scope execs the shell in place: wait4 sees the test.
        # OOMPolicy=continue: the kernel kills the process that ran over, and
        # the shell lives to read the counters (the default stops the scope).
        # --collect: a scope the kernel's limit failed is not kept loaded.
        parent = os.environ.get('BESS_TEST_MEM_SLICE')
        status, maxrss = spawn(
            ['systemd-run', '--user', '--scope', '-q', '--collect', f'--unit={unit}',
             *([f'--slice={parent}'] if parent else []),
             '-p', f'MemoryMax={limit_mb}M', '-p', 'MemorySwapMax=0',
             '-p', 'OOMPolicy=continue',
             '--', 'sh', '-c', INNER, 'sh', out, *cmd], env)
        lines = read_lines(out)
    if not lines or lines[0] != 'started':
        return None
    fields = dict(line.split(' ', 1) for line in lines[1:] if ' ' in line)
    if 'peak' not in fields:
        # The shell was killed with the test: only the kernel's limit does that.
        return status, -1, 0, maxrss, 1

    def count(key):
        value = fields.get(key, '').strip()
        return int(value) if value.lstrip('-').isdigit() else -1

    # memory.stat's file includes shmem. -1: a kernel before 5.19 has no
    # memory.peak.
    cache = max(count('file') - max(count('shmem'), 0), 0)
    return status, count('peak'), cache, maxrss, max(count('oom_kill'), 0)


def exit_like(status):
    """Exit as the test did: a signal again as a signal, so meson names it."""
    sys.stdout.flush()
    sys.stderr.flush()
    if status > 128 and status - 128 < signal.NSIG:
        sig = status - 128
        if sig not in (signal.SIGKILL, signal.SIGSTOP):
            signal.signal(sig, signal.SIG_DFL)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # the test dumped, if anyone
        os.kill(os.getpid(), sig)
    sys.exit(status)


def run(cmd, no_scope=False):
    # A script is named for itself, not for its interpreter.
    named = cmd[1:2] if cmd and os.path.basename(cmd[0]).startswith('python') else cmd
    name = os.path.basename(named[0]) if named else '?'
    declared = os.environ.get('BESS_TEST_MEM_MB', '')
    measure = os.environ.get('BESS_TEST_MEM_MEASURE') == '1'
    # Calibration measures a test that has no declaration yet.
    if not measure and (not declared.isdigit() or int(declared) <= 0):
        print(f'run_capped: {name}: no memory declaration (BESS_TEST_MEM_MB={declared!r}); '
              'every test declares one in tests/memory/meson.build', file=sys.stderr)
        return 1
    declared_mb = int(declared) if declared.isdigit() else 0
    no_scope = no_scope or os.environ.get('BESS_TEST_MEM_NO_SCOPE') == '1'

    fd, report = tempfile.mkstemp(prefix='run_capped.report.')
    os.close(fd)
    env = dict(os.environ, BESS_TEST_MEM_REPORT=report)
    try:
        limit_mb = kill_limit_mb(declared_mb, measure)
        scoped = None if no_scope else run_in_scope(cmd, env, limit_mb)
        if scoped is not None:
            status, cgroup, cache, maxrss, oom = scoped
        else:
            (status, maxrss), cgroup, cache, oom = spawn(cmd, env), -1, 0, 0
        escaped = []
        for line in read_lines(report):
            label, _, value = line.rpartition(' ')
            if value.lstrip('-').isdigit() and int(value) >= 0:
                escaped.append((label, int(value)))
    finally:
        os.unlink(report)

    own = max(cgroup - cache, 0)
    tree = own + sum(v for _, v in escaped)
    peak = max(tree, maxrss)
    parts = [f'cgroup memory.peak {cgroup / MIB:.1f} less page cache {cache / MIB:.1f}'
             if cgroup >= 0 else
             'no cgroup memory.peak' if scoped is not None else 'no cgroup']
    parts += [f'{label} {value / MIB:.1f}' for label, value in escaped]
    detail = ' + '.join(parts)
    print(f'run_capped: {name}: declared {declared_mb} MiB, peak {peak / MIB:.1f} MiB '
          f'({detail}; wait4 ru_maxrss {maxrss / MIB:.1f}; '
          f'kill limit {limit_mb if scoped is not None else "none"})',
          file=sys.stderr)
    if oom:
        print(f'run_capped: {name}: FAILED: killed by the kernel at its cgroup limit '
              f'({limit_mb} MiB; declared {declared_mb} MiB)', file=sys.stderr)
        return status or 1
    if not measure and peak > declared_mb * MIB:
        print(f'run_capped: {name}: FAILED: declared {declared_mb} MiB, '
              f'peak {peak / MIB:.1f} MiB', file=sys.stderr)
        return status or 1
    return status


PEAK = re.compile(r'declared (\d+) MiB, peak ([0-9.]+) MiB')


def self_test(hog):
    """The mechanism, by bytes: a process that touches more than its
    declaration fails with "declared X, peak Y", the same process under its
    bound passes, and the peak reported is at least what it touched; in a
    scope and with wait4 alone. HOG touches argv[1] bytes and exits 0."""
    failures = []
    escaped = []  # the scope cases' peaks: cgroups outside this test's own

    def case(touch_mb, declared_mb, no_scope, want):
        env = dict(os.environ, BESS_TEST_MEM_MB=str(declared_mb))
        for name in ('BESS_TEST_MEM_MEASURE', 'BESS_TEST_MEM_NO_SCOPE', 'BESS_TEST_MEM_REPORT'):
            env.pop(name, None)
        cmd = [sys.executable, os.path.abspath(__file__)]
        cmd += ['--no-scope'] if no_scope else []
        cmd += [hog, str(touch_mb * MIB)]
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        mode = 'wait4' if no_scope else 'scope'
        got = 'pass' if r.returncode == 0 else ('oom' if 'killed by the kernel' in r.stderr
                                                else 'over')
        m = PEAK.search(r.stderr)
        peak = float(m.group(2)) if m else -1.0
        print(f'{mode}: touch {touch_mb} MiB, declared {declared_mb} MiB: {got}, '
              f'peak {peak:.1f} MiB')
        if not no_scope:
            escaped.append(int(peak * MIB))
        if got != want:
            failures.append(f'{mode} {touch_mb}/{declared_mb}: want {want}, got {got}: '
                            f'{r.stderr.strip()}')
        elif not m or int(m.group(1)) != declared_mb:
            failures.append(f'{mode} {touch_mb}/{declared_mb}: no "declared X MiB, peak Y MiB"')
        elif want != 'oom' and peak < touch_mb:
            failures.append(f'{mode} {touch_mb}/{declared_mb}: peak {peak} MiB is below the '
                            f'{touch_mb} MiB touched')
        if want == 'oom' and r.returncode != -signal.SIGKILL:
            failures.append(f'{mode} {touch_mb}/{declared_mb}: kernel kill was not '
                            f'propagated as SIGKILL (status {r.returncode})')

    scope_ok = bool(shutil.which('systemd-run')) and subprocess.run(
        ['systemd-run', '--user', '--scope', '-q', 'true'], capture_output=True).returncode == 0
    # Declared 48 MiB: the kernel's limit is max(48 * 1.5, 48 + 32) = 80 MiB.
    for no_scope in ([False, True] if scope_ok else [True]):
        case(32, 64, no_scope, 'pass')   # under its bound
        case(64, 48, no_scope, 'over')   # above it, below the kernel's limit: measured, failed
        if not no_scope:
            case(160, 48, no_scope, 'oom')  # far above: the kernel stops it at the limit
        else:
            case(160, 48, no_scope, 'over')  # no cgroup: runs to the end, then failed
    if not scope_ok:
        print('no user manager: the scope cases did not run')
        if os.environ.get('BESS_TEST_MEM_REQUIRE_SCOPE') == '1':
            failures.append('BESS_TEST_MEM_REQUIRE_SCOPE=1 and no scope could be made')
    report = os.environ.get('BESS_TEST_MEM_REPORT')
    if report and escaped:
        with open(report, 'a', encoding='utf-8') as f:
            f.write(f'self-test scope cases (largest; one at a time) {max(escaped)}\n')
    for f in failures:
        print('FAIL', f)
    return 1 if failures else 0


def main():
    args = sys.argv[1:]
    if args[:1] == ['--self-test']:
        if len(args) != 2:
            sys.exit('usage: run_capped.py --self-test HOG')
        return self_test(args[1])
    no_scope = args[:1] == ['--no-scope']
    if no_scope:
        args = args[1:]
    if args[:1] == ['--']:
        args = args[1:]
    if not args:
        sys.exit(__doc__)
    exit_like(run(args, no_scope))


if __name__ == '__main__':
    sys.exit(main())
