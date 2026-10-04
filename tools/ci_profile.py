#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build and verify BESS exactly the way the gating CI jobs do.

CI (.github/workflows/ci.yml) and a developer's machine run this one script, so
the configuration cannot drift: the Meson options, the DPDK build (profile,
CPU floor, AF_XDP, the compiler that builds it) and the verification steps are
defined here and nowhere else.

    tools/ci_profile.py all --compiler gcc       # what the "gcc" lane does
    tools/ci_profile.py all --compiler clang     # what the "clang" lane does
    tools/ci_profile.py all --compiler gcc --arch-generic   # the "gcc-generic" lane
    tools/ci_profile.py info --compiler clang    # resolved setup and divergences
    tools/ci_profile.py build --compiler gcc     # one step (they are re-runnable)
    tools/ci_profile.py check-cpu                # does this CPU meet the floor?

Steps, in CI order: bootstrap (DPDK), configure, build, verify-dpdk, layers,
test, verify-install, clean-tree. `all` runs them in that order. clean-tree
fails if the build or the tests left the source tree changed (a modified or
untracked, non-ignored file): a build writes only to its build directory. Run
alone it requires a clean tree; under `all` it compares with the tree as `all`
found it, so uncommitted work of your own is not blamed on the build.

The CPU floor is per architecture (CPU_FLOOR): the machine's architecture picks
it, for BESS (-Dcpu) and for DPDK (tools/bootstrap_dpdk.py). `--arch` shows
another architecture's commands with --dry-run or info; it cannot cross-build.

What this cannot reproduce on a developer machine, and `info` says so:
  * the exact compiler versions, when gcc-14 / clang-19 are not installed (the
    nearest installed compiler is used and flagged);
  * the GitHub runner's 512 reserved hugepages (tests here use the no-hugepage
    mode);
  * the runner's CPU (the code is built for the floor, but a CPU that lacks it
    cannot run the result; check-cpu says whether this one has it).

Local runs never exceed 8 build jobs (docs: MODERNIZATION.md "Build profiles").
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The compiler pin of each lane family. The arm64, generic and sanitizer lanes
# (<family>-arm64, ...) use their family's pin; `check-pins` checks LANES
# against it, and CI runs it.
PINS = {
    'gcc': ('gcc-14', 'g++-14'),
    'clang': ('clang-19', 'clang++-19'),
}
LANE_SUFFIXES = ('-arm64', '-generic', '-asan', '-tsan')
# Sanitizer lanes (M22, D-072): --sanitize picks the build and the test step.
SANITIZERS = {'address': '-asan', 'thread': '-tsan'}
# TSan needs DPDK's C11 atomics and no x86 inline asm in the headers BESS
# inlines, or it reports the ring and spinlock code as races.
TSAN_DPDK_ARGS = '-DRTE_USE_C11_MEM_MODEL -DRTE_FORCE_INTRINSICS'
# The CI lanes: the build-and-test matrix of .github/workflows/ci.yml, which
# reads it from `ci_profile.py lanes`. The ubuntu-24.04 x86 pair gates. The
# others are experimental and must not block a merge: the ubuntu-26.04 lanes
# show toolchain and library drift (GCC 15, Clang 22) before it reaches the
# gating lanes; the arm64 and generic lanes are new (M21) and gate once they
# have been green. `push_only` lanes run on pushes to develop/master and tags,
# not on pull requests: they catch drift and portability breaks a release
# later, and leaving them out halves a pull request's runner time. The
# sanitizer lanes run on pull requests.
LANES = [
    # GCC 14 (Ubuntu 24.04 noble-updates): the distro default is 13, which
    # lacks C++23 features the codebase may use (deducing this).
    {'name': 'gcc', 'os': 'ubuntu-24.04', 'cc': 'gcc-14', 'cxx': 'g++-14'},
    # Clang 19 updates __cpp_concepts for libstdc++'s C++23 <expected>.
    {'name': 'clang', 'os': 'ubuntu-24.04', 'cc': 'clang-19', 'cxx': 'clang++-19'},
    # x86 with -Darch_generic=true: core/arch/ takes the portable paths every
    # other architecture runs, so they are built (benchmarks included) and
    # tested; the gating lanes keep testing the x86 paths that ship.
    {'name': 'gcc-generic', 'os': 'ubuntu-24.04', 'cc': 'gcc-14', 'cxx': 'g++-14',
     'lane_args': '--arch-generic', 'experimental': True, 'push_only': True},
    # arm64: GitHub's ubuntu-24.04-arm runners (Azure Cobalt 100, Neoverse
    # N2), the same compilers, DPDK bootstrap and steps as the gating pair,
    # built for the armv8.2-a floor. AF_XDP stays required: Ubuntu 24.04 ships
    # libxdp-dev/libbpf-dev for arm64.
    {'name': 'gcc-arm64', 'os': 'ubuntu-24.04-arm', 'cc': 'gcc-14', 'cxx': 'g++-14',
     'experimental': True, 'push_only': True},
    {'name': 'clang-arm64', 'os': 'ubuntu-24.04-arm', 'cc': 'clang-19', 'cxx': 'clang++-19',
     'experimental': True, 'push_only': True},
    # Sanitizers (M22, D-072): ASan+UBSan over the unit, architecture, plugin
    # and fuzz-corpus suites; TSan over the concurrency tests in
    # tools/sanitizers/tsan_tests.txt. Experimental until green, then gating.
    {'name': 'clang-asan', 'os': 'ubuntu-24.04', 'cc': 'clang-19', 'cxx': 'clang++-19',
     'lane_args': '--sanitize address', 'experimental': True},
    {'name': 'clang-tsan', 'os': 'ubuntu-24.04', 'cc': 'clang-19', 'cxx': 'clang++-19',
     'lane_args': '--sanitize thread', 'experimental': True},
    {'name': 'gcc-u26', 'os': 'ubuntu-26.04', 'cc': 'gcc-15', 'cxx': 'g++-15',
     'experimental': True, 'push_only': True},
    {'name': 'clang-u26', 'os': 'ubuntu-26.04', 'cc': 'clang-22', 'cxx': 'clang++-22',
     'experimental': True, 'push_only': True},
]
FALLBACK = {
    'gcc': ('gcc', 'g++'),
    'clang': ('clang', 'clang++'),
}

# The portable floor per architecture (`uname -m`), passed as -march=<cpu>.
#   x86_64   x86-64-v3 (AVX2, BMI1/2, FMA, MOVBE; D-016). GitHub's x64 runners
#            (AMD EPYC, Zen 3+) have it; v4 (AVX-512) they do not.
#   aarch64  armv8.2-a (LSE atomics, CRC32, RDM): Neoverse N1 (Graviton2,
#            Ampere Altra) and everything newer, including GitHub's arm64
#            runners (Azure Cobalt 100, Neoverse N2). Not Cortex-A72
#            (Graviton1, Raspberry Pi 4), which is armv8.0.
CPU_FLOOR = {
    'x86_64': 'x86-64-v3',
    'aarch64': 'armv8.2-a',
}
# The /proc/cpuinfo features -march=armv8.2-a lets the compiler use.
ARM64_FLOOR_FEATURES = ('fp', 'asimd', 'crc32', 'atomics', 'asimdrdm')
DPDK_PROFILE = 'bess'
STANDALONE_PLUGINS = ('standalone_pass', 'standalone_macswap',
                      'standalone_range_gate', 'standalone_flow_count',
                      'standalone_appliance')
LOCAL_JOB_CAP = 8


def host_arch():
    return platform.machine()


def cpu_floor(arch):
    if arch not in CPU_FLOOR:
        raise SystemExit(f'no CPU floor for {arch}: BESS builds on {", ".join(CPU_FLOOR)}')
    return CPU_FLOOR[arch]


def meson_options(s):
    options = [
        f'-Dcpu={s.cpu}',
        '-Daf_xdp=required',
        '-Dbuildtype=debugoptimized',
        '-Dbuild_sample_plugin=true',
        # build_benchmarks stays at its default (true): CI compiles the
        # benchmarks, so every other build of this profile must as well.
    ]
    if s.arch_generic:
        options.append('-Darch_generic=true')
    if s.sanitize == 'address':
        # Benchmarks are timing code: built and run in the other lanes.
        options += ['-Db_sanitize=address,undefined', '-Db_lundef=false',
                    '-Dbuild_benchmarks=false']
    elif s.sanitize == 'thread':
        options += ['-Db_sanitize=thread', '-Db_lundef=false',
                    '-Dbuild_benchmarks=false',
                    f'-Dcpp_args={TSAN_DPDK_ARGS}', f'-Dc_args={TSAN_DPDK_ARGS}']
    return options


class Setup:
    def __init__(self, args):
        self.dry_run = args.dry_run
        self.in_ci = os.environ.get('GITHUB_ACTIONS') == 'true'
        self.notes = []
        self.tree_before = {}  # the tree as `all` found it; empty means "must be clean"
        self.arch = args.arch or host_arch()
        if self.arch != host_arch() and not (args.dry_run or args.step == 'info'):
            raise SystemExit(f'--arch {self.arch} on a {host_arch()} machine: only '
                             '--dry-run and info can show another architecture')
        self.cpu = cpu_floor(self.arch)
        self.arch_generic = args.arch_generic
        self.sanitize = args.sanitize
        family = args.compiler
        if args.cc or args.cxx:
            if not (args.cc and args.cxx and args.name):
                raise SystemExit('--cc, --cxx and --name go together')
            self.cc, self.cxx, self.name = args.cc, args.cxx, args.name
            self.family = 'clang' if 'clang' in args.cxx else 'gcc'
        else:
            if family is None:
                raise SystemExit('give --compiler gcc|clang (or --cc/--cxx/--name)')
            self.family = family
            pinned = PINS[family]
            if shutil.which(pinned[1]) and shutil.which(pinned[0]):
                self.cc, self.cxx = pinned
            else:
                self.cc, self.cxx = FALLBACK[family]
                self.notes.append(
                    f'CI uses {pinned[0]}/{pinned[1]}; they are not installed, '
                    f'so {self.cxx} ({self.version(self.cxx)}) is used instead. '
                    'A diagnostic that exists in only one compiler version can '
                    'still differ from CI.')
            self.name = args.name or (f'ci-{family}' + ('-generic' if self.arch_generic else '')
                                      + SANITIZERS.get(self.sanitize, ''))
        self.build_dir = ROOT / 'build' / self.name
        self.stage = ROOT / 'build' / f'stage-{self.name}'
        self.standalone = ROOT / 'build' / f'standalone-{self.name}'
        self.stage_pkgconfig = ROOT / 'build' / f'stage-pkgconfig-{self.name}'
        self.variant = self.name
        jobs = args.jobs or (4 if self.in_ci else min(LOCAL_JOB_CAP, os.cpu_count() or 1))
        if not self.in_ci and jobs > LOCAL_JOB_CAP:
            raise SystemExit(f'refusing --jobs {jobs}: local builds are capped at '
                             f'{LOCAL_JOB_CAP}')
        self.jobs = jobs
        launcher = ''
        # Local runs use ccache when installed; CI when the workflow restores
        # a cache directory (CCACHE_DIR). It does not change the generated code.
        if (not args.no_ccache and shutil.which('ccache')
                and (not self.in_ci or os.environ.get('CCACHE_DIR'))):
            launcher = 'ccache '
        self.env = os.environ.copy()
        self.env['CC'] = launcher + self.cc
        self.env['CXX'] = launcher + self.cxx
        self.env['CPU'] = self.cpu
        self.env['AF_XDP'] = 'required'

    @staticmethod
    def version(tool):
        try:
            out = subprocess.run([tool, '--version'], capture_output=True,
                                 text=True, check=True).stdout
            return out.splitlines()[0]
        except (OSError, subprocess.CalledProcessError):
            return 'not installed'

    def run(self, command, env=None, cwd=ROOT, **kwargs):
        shown = ' '.join(str(c) for c in command)
        print(f'+ {shown}', flush=True)
        if self.dry_run:
            return subprocess.CompletedProcess(command, 0, '', '')
        return subprocess.run([str(c) for c in command], env=env or self.env,
                              cwd=cwd, check=True, **kwargs)

    def dpdk_pkgconfig(self):
        out = subprocess.run(
            [sys.executable, ROOT / 'tools' / 'bootstrap_dpdk.py',
             '--variant', self.variant, '--print-pkg-config-path'],
            capture_output=True, text=True, check=True, env=self.env)
        return out.stdout.strip()

    def dpdk_libdir(self):
        if self.dry_run:
            return ''
        return subprocess.run(['pkg-config', '--variable=libdir', 'libdpdk'],
                              capture_output=True, text=True, check=True,
                              env=self.env_with_dpdk()).stdout.strip()

    def env_with_dpdk(self, *extra):
        env = self.env.copy()
        paths = [*extra, self.dpdk_pkgconfig()]
        if env.get('PKG_CONFIG_PATH'):
            paths.append(env['PKG_CONFIG_PATH'])
        env['PKG_CONFIG_PATH'] = os.pathsep.join(paths)
        return env


def step_bootstrap(s):
    s.run([sys.executable, ROOT / 'tools' / 'bootstrap_dpdk.py',
           '--af-xdp', 'required', '--cpu', s.cpu, '--profile', DPDK_PROFILE,
           '--variant', s.variant, '-j', s.jobs])


def step_configure(s):
    command = ['meson', 'setup']
    if (s.build_dir / 'meson-private' / 'coredata.dat').exists():
        command.append('--reconfigure')
    command += [s.build_dir, *meson_options(s)]
    s.run(command, env=s.env_with_dpdk())


def step_build(s):
    s.run(['meson', 'compile', '-C', s.build_dir, '-j', s.jobs],
          env=s.env_with_dpdk())


def step_verify_dpdk(s):
    env = s.env_with_dpdk()
    if s.dry_run:
        print('+ pkg-config --variable=libdir libdpdk; test librte_net_af_xdp.{so,a}')
        return
    libdir = Path(subprocess.run(['pkg-config', '--variable=libdir', 'libdpdk'],
                                 capture_output=True, text=True, check=True,
                                 env=env).stdout.strip())
    for name in ('librte_net_af_xdp.so', 'librte_net_af_xdp.a'):
        if not (libdir / name).is_file():
            raise SystemExit(f'missing {libdir / name}')
    print('AF_XDP artifacts: present')


def step_layers(s):
    s.run([sys.executable, ROOT / 'tools' / 'check_includes.py', '--self-test'])
    s.run([sys.executable, ROOT / 'tools' / 'check_includes.py'])
    s.run([sys.executable, ROOT / 'tools' / 'check_arch.py', '--self-test'])
    s.run([sys.executable, ROOT / 'tools' / 'check_arch.py'])


def symbolizer():
    # Sanitizer reports name frames only through llvm-symbolizer, and the TSan
    # suppressions match on function names: without it they match nothing.
    for path in (shutil.which('llvm-symbolizer'), '/usr/lib/llvm-19/bin/llvm-symbolizer',
                 shutil.which('llvm-symbolizer-19')):
        if path and os.access(path, os.X_OK):
            return path
    raise SystemExit('llvm-symbolizer not found (env/install-deps.sh installs llvm-19)')


def step_test(s):
    if s.sanitize == 'address':
        # Every unit, architecture, plugin and fuzz-corpus test under ASan and
        # UBSan, halting on the first report. Explicit exclusions: benchmarks
        # (not built) and the daemon suites `python` and `integration` (bessd
        # under ASan is not yet a supported mode; D-072).
        env = s.env_with_dpdk()
        env['ASAN_OPTIONS'] = ('detect_leaks=1:halt_on_error=1:abort_on_error=1'
                               ':external_symbolizer_path=' + symbolizer())
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1:external_symbolizer_path=' + symbolizer()
        s.run(['meson', 'test', '-C', s.build_dir, '--no-rebuild', '--print-errorlogs',
               '--no-suite', 'python', '--no-suite', 'integration',
               '--timeout-multiplier', '6'], env=env)
        return
    if s.sanitize == 'thread':
        step_test_tsan(s)
        return
    # Benchmarks are built in every lane; pull requests do not run them (at
    # 0.001 s per case they measure nothing, and their setup was 80% of the
    # test step). Pushes and the nightly run execute them.
    skip = (['--no-suite', 'benchmarks']
            if os.environ.get('GITHUB_EVENT_NAME') == 'pull_request' else [])
    s.run(['meson', 'test', '-C', s.build_dir, '--no-rebuild', '--print-errorlogs', *skip],
          env=s.env_with_dpdk())


def step_test_tsan(s):
    env = s.env_with_dpdk()
    env['TSAN_OPTIONS'] = ('suppressions=' + str(ROOT / 'tools/sanitizers/tsan.supp')
                           + ' halt_on_error=1 second_deadlock_stack=1'
                           + ' external_symbolizer_path=' + symbolizer())
    env['LD_LIBRARY_PATH'] = os.pathsep.join(
        filter(None, [s.dpdk_libdir(), env.get('LD_LIBRARY_PATH')]))
    tests = []
    for line in (ROOT / 'tools/sanitizers/tsan_tests.txt').read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            name, *rest = line.split()
            tests.append((name, rest[0] if rest else '*'))
    s.run(['meson', 'compile', '-C', s.build_dir, '-j', s.jobs,
           *[f'core/{name}' for name, _ in tests]], env=env)
    for name, test_filter in tests:
        s.run([s.build_dir / 'core' / name, f'--gtest_filter={test_filter}'], env=env,
              cwd=s.build_dir / 'core')


def step_verify_install(s):
    if s.sanitize:
        print('skipped: a sanitizer lane installs nothing (the gating lanes verify the install)')
        return
    env = s.env_with_dpdk()
    s.run(['meson', 'install', '-C', s.build_dir, '--destdir', s.stage], env=env)
    if s.dry_run:
        print('+ check installed headers, build standalone plugins, check bessd')
        return
    pc = next(s.stage.rglob('bess-dev.pc'), None)
    if pc is None:
        raise SystemExit('bess-dev.pc was not installed')
    # The installed bess-dev.pc names the real prefix (/usr/local). Consumers
    # here must see the stage and nothing else: a copy whose prefix is the
    # stage's, first on the path, so a BESS install already on the machine
    # cannot supply a header (its older Module layout once made every
    # installed-tree plugin corrupt bessd's heap on a developer machine).
    staged_pc = s.stage_pkgconfig
    staged_pc.mkdir(parents=True, exist_ok=True)
    text = pc.read_text()
    prefix = next((line.split('=', 1)[1] for line in text.splitlines()
                   if line.startswith('prefix=')), None)
    if prefix is None:
        raise SystemExit(f'{pc} has no prefix= line')
    stage_prefix = s.stage / prefix.lstrip('/')
    (staged_pc / 'bess-dev.pc').write_text(
        text.replace(f'prefix={prefix}', f'prefix={stage_prefix}', 1))
    env['PKG_CONFIG_PATH'] = os.pathsep.join([str(staged_pc), env['PKG_CONFIG_PATH']])
    env['CXX'] = s.env['CXX']
    include = stage_prefix / 'include/bess'
    s.run([sys.executable, ROOT / 'tools' / 'check_installed_headers.py',
           '--include-dir', include / 'core', '--march', s.cpu], env=env)
    # A fresh builddir: a configured one keeps the bess-dev it resolved before.
    shutil.rmtree(s.standalone, ignore_errors=True)
    s.run(['meson', 'setup', s.standalone, 'examples/standalone_plugin'], env=env)
    s.run(['meson', 'compile', '-C', s.standalone], env=env)
    for plugin in STANDALONE_PLUGINS:
        so = s.standalone / f'lib{plugin}.so'
        if not so.is_file():
            raise SystemExit(f'{so} was not built')
        symbols = subprocess.run(['nm', '-D', '--defined-only', so],
                                 capture_output=True, text=True, check=True).stdout
        if 'bess_plugin_descriptor_v1' not in symbols:
            raise SystemExit(f'{so} does not export bess_plugin_descriptor_v1')
    bessd = list(s.stage.glob('**/bin/bessd'))
    plugins = list(s.stage.glob('**/bess/modules/libsequential_update.so'))
    if len(bessd) != 1 or len(plugins) != 1 or not os.access(bessd[0], os.X_OK):
        raise SystemExit(f'unexpected install: bessd={bessd} plugins={plugins}')
    s.run([sys.executable, ROOT / 'tools' / 'check_installed_pybess.py',
           '--root', s.stage / 'usr/local/share/bess'], env=env)
    # Release metadata (M26): the install says what it is made of.
    info_files = list(s.stage.glob('**/share/bess/build-info.json'))
    sbom_files = list(s.stage.glob('**/share/doc/bess/bess.spdx.json'))
    if len(info_files) != 1 or len(sbom_files) != 1:
        raise SystemExit(f'missing release metadata: {info_files} {sbom_files}')
    info = json.loads(info_files[0].read_text())
    for key in ('version', 'commit', 'plugin_api_version', 'compilers', 'options', 'dpdk',
                'dependencies'):
        if key not in info:
            raise SystemExit(f'build-info.json lacks {key}')
    if not isinstance(info['plugin_api_version'], int) or not info['dpdk'].get('sha256'):
        raise SystemExit(f"build-info.json is incomplete: plugin_api_version="
                         f"{info['plugin_api_version']!r} dpdk={info['dpdk']!r}")
    if os.environ.get('GITHUB_ACTIONS') and info['commit'] == 'unknown':
        raise SystemExit('build-info.json has no commit in a git checkout (git failed?)')
    sbom = json.loads(sbom_files[0].read_text())
    if sbom.get('spdxVersion') != 'SPDX-2.3' or not any(
            p['name'] == 'dpdk' and p.get('checksums') for p in sbom.get('packages', [])):
        raise SystemExit('bess.spdx.json is not an SPDX 2.3 document with the DPDK pin')
    # The installed-tree plugins load into the staged bessd and move packets,
    # driven by the installed Python client (M23).
    client_root = str(s.stage / 'usr/local/share/bess')
    s.run([sys.executable, ROOT / 'tools' / 'check_standalone_plugins.py',
           '--bessd', bessd[0], '--plugin-dir', s.standalone],
          env={**env, 'PYTHONPATH': client_root, 'BESS_PROTOBUF_ROOT': client_root,
               # bessd links DPDK's shared libraries, which are not installed
               'LD_LIBRARY_PATH': os.pathsep.join(
                   filter(None, [s.dpdk_libdir(), env.get('LD_LIBRARY_PATH')]))})
    # A third-party library on the installed primitives, built and tested
    # without bessd (M23, persona E).
    battery = ROOT / 'build' / f'battery-{s.name}'
    shutil.rmtree(battery, ignore_errors=True)
    s.run(['meson', 'setup', battery, 'examples/sdk_battery'], env=env)
    s.run(['meson', 'test', '-C', battery, '--print-errorlogs'], env=env)
    # API samples, one per public library, compiled from the install (M23).
    samples = ROOT / 'build' / f'samples-{s.name}'
    shutil.rmtree(samples, ignore_errors=True)
    s.run(['meson', 'setup', samples, 'examples/sdk_samples'], env=env)
    s.run(['meson', 'compile', '-C', samples], env=env)


def tree_state():
    """{path: content hash} of every modified or untracked non-ignored file."""
    out = subprocess.run(
        ['git', 'status', '--porcelain=v1', '-z', '--untracked-files=all'],
        cwd=ROOT, capture_output=True, check=True).stdout.decode()
    state = {}
    fields = out.split('\0')
    i = 0
    while i < len(fields):
        entry = fields[i]
        i += 1
        if not entry:
            continue
        path = entry[3:]
        if entry[0] in 'RC':
            i += 1  # the origin path of a rename or copy follows
        file = ROOT / path
        state[path] = (hashlib.sha256(file.read_bytes()).hexdigest()
                       if file.is_file() else 'absent')
    return state


def step_clean_tree(s):
    if s.dry_run:
        print('+ git status --porcelain (fails if the build changed the source tree)')
        return
    now = tree_state()
    changed = {path for path in now.keys() | s.tree_before.keys()
               if now.get(path) != s.tree_before.get(path)}
    if changed:
        listing = '\n'.join(f'  {path}' for path in sorted(changed))
        raise SystemExit(
            'the source tree differs from how the run found it (alone: from HEAD); '
            'a build and its tests must write only to the build directory. Fix '
            'the rule that wrote these, or ignore the generated file:\n' + listing)
    print('source tree unchanged')


def step_tidy(s):
    # The curated clang-tidy gate (D-072) needs a clang compile database: it
    # runs in the clang ASan lane, the one clang build with every library.
    if s.sanitize != 'address':
        print('skipped: clang-tidy runs in the clang-asan lane')
        return
    s.run([sys.executable, ROOT / 'tools' / 'check_tidy.py', '--self-test'])
    files = []
    changed = pull_request_changes()
    if changed is not None:
        if any(path.endswith(('.h', '.hh', '.hpp')) for path in changed) or any(
                Path(path).name in ('meson.build', '.clang-tidy', 'tidy_baseline.json',
                                    'check_tidy.py') for path in changed):
            print('tidy: a header or the tidy configuration changed: every source')
        else:
            sources = [path for path in changed if path.endswith(('.cc', '.cpp'))]
            if not sources:
                print('skipped: the pull request changes no C++ source')
                return
            print(f'tidy: the {len(sources)} changed source(s) only (pushes check all)')
            files = ['--files', '|'.join(re.escape(path) + '$' for path in sources)]
    s.run([sys.executable, ROOT / 'tools' / 'check_tidy.py', '--build-dir', s.build_dir,
           '--jobs', s.jobs, '--strict', *files], env=s.env_with_dpdk())


def pull_request_changes():
    """The files a pull request changes, or None outside one. CI checks out
    the merge commit, whose first parent is the base branch tip."""
    if os.environ.get('GITHUB_EVENT_NAME') != 'pull_request':
        return None
    out = subprocess.run(['git', 'diff', '--name-only', 'HEAD^1', 'HEAD'], cwd=ROOT,
                         capture_output=True, text=True)
    if out.returncode != 0:
        print('could not list the pull request\'s changes; checking everything')
        return None
    return [line for line in out.stdout.splitlines() if line]


STEPS = [
    ('bootstrap', step_bootstrap),
    ('configure', step_configure),
    ('build', step_build),
    ('verify-dpdk', step_verify_dpdk),
    ('layers', step_layers),
    ('tidy', step_tidy),
    ('test', step_test),
    ('verify-install', step_verify_install),
    ('clean-tree', step_clean_tree),
]

# The release job (publish-release in ci.yml): a static standalone bessd built
# with the same compiler as the gating gcc lane, the `full` DPDK profile, and
# no benchmarks or sample plugin. It shares this script's compiler and CPU
# authority; only the options below are release-specific. Not part of `all`.
def release_meson_options(s):
    return [
        f'-Dcpu={s.cpu}',
        '-Dbuildtype=release',
        '-Dstatic_binary=standalone',
        '-Daf_xdp=required',
        '-Dbuild_benchmarks=false',
        '-Dbuild_sample_plugin=false',
    ]


def step_release_bootstrap(s):
    s.run([sys.executable, ROOT / 'tools' / 'bootstrap_dpdk.py',
           '--af-xdp', 'required', '--cpu', s.cpu, '--profile', 'full',
           '--variant', s.variant, '-j', s.jobs])


def step_release_configure(s):
    command = ['meson', 'setup']
    if (s.build_dir / 'meson-private' / 'coredata.dat').exists():
        command.append('--reconfigure')
    command += [s.build_dir, *release_meson_options(s)]
    s.run(command, env=s.env_with_dpdk())


def step_release_build(s):
    s.run(['meson', 'compile', '-C', s.build_dir, '-j', s.jobs, 'core/bessd'],
          env=s.env_with_dpdk())


def step_release_verify(s):
    bessd = s.build_dir / 'core' / 'bessd'
    if s.dry_run:
        print(f'+ ldd {bessd} must name no librte_*')
        return
    if not bessd.is_file():
        raise SystemExit(f'{bessd} was not built')
    linked = subprocess.run(['ldd', bessd], capture_output=True, text=True).stdout
    dynamic_dpdk = [l for l in linked.splitlines() if 'librte' in l.lower()]
    if dynamic_dpdk:
        raise SystemExit('bessd has unexpected dynamic DPDK dependencies:\n'
                         + '\n'.join(dynamic_dpdk))
    print('bessd links no dynamic DPDK library')


RELEASE_STEPS = [
    ('release-bootstrap', step_release_bootstrap),
    ('release-configure', step_release_configure),
    ('release-build', step_release_build),
    ('release-verify', step_release_verify),
]


def info(s):
    print(f'lane            {s.name} ({s.family})')
    print(f'compiler        {s.cxx}: {s.version(s.cxx)}')
    print(f'                {s.cc}: {s.version(s.cc)}')
    print(f'architecture    {s.arch}, cpu floor {s.cpu}'
          + (', BESS_ARCH_GENERIC' if s.arch_generic else '')
          + (f', sanitizer {s.sanitize}' if s.sanitize else ''))
    print(f'build dir       {s.build_dir}')
    print(f'DPDK            profile {DPDK_PROFILE}, cpu {s.cpu}, variant {s.variant}')
    print(f'meson options   {" ".join(meson_options(s))}'
          + ('' if s.sanitize else ' (benchmarks on, default)'))
    print(f'jobs            {s.jobs}')
    divergences = list(s.notes)
    try:
        huge = int(Path('/proc/sys/vm/nr_hugepages').read_text())
    except OSError:
        huge = 0
    if huge < 512 and not s.in_ci:
        divergences.append(f'{huge} hugepages reserved (CI reserves 512); tests '
                           'run in the no-hugepage mode')
    print('divergences from CI:' + ('' if divergences else ' none'))
    for d in divergences:
        print(f'  - {d}')


def ci_lanes(event):
    """The build-and-test matrix for a GitHub event, as the workflow's
    `matrix=` output."""
    include = [{k: v for k, v in lane.items() if k != 'push_only'}
               for lane in LANES if not (event == 'pull_request' and lane.get('push_only'))]
    for lane in include:
        lane.setdefault('lane_args', '')
        lane.setdefault('experimental', False)
    return 'matrix=' + json.dumps({'include': include}, separators=(',', ':'))


def check_pins():
    lanes = {m['name']: (m['cc'], m['cxx']) for m in LANES}
    gating = {m['name'] for m in LANES if not m.get('experimental')}
    bad = []
    for family, pair in PINS.items():
        if family not in gating or lanes[family] != pair:
            bad.append(f'{family}: PINS has {pair}, the gating lane has '
                       f'{lanes.get(family) if family in gating else None}')
        for suffix in LANE_SUFFIXES:
            name = family + suffix
            if name in lanes and lanes[name] != pair:
                bad.append(f'{name}: PINS has {pair}, the lane has {lanes[name]}')
    known = {family + suffix for family in PINS for suffix in ('',) + LANE_SUFFIXES}
    extra = gating - known
    if extra:
        bad.append(f'gating lanes without a pin: {sorted(extra)}')
    if bad:
        raise SystemExit('LANES and PINS disagree:\n  ' + '\n  '.join(bad))
    pinned = sorted(lanes.keys() & known)
    print('pins match the CI lanes:', ', '.join(pinned),
          f'(gating: {", ".join(sorted(gating))})')


def check_cpu(arch):
    """Fail unless this machine's CPU has the architecture's floor."""
    cpu = cpu_floor(arch)
    prefix = '::error::' if os.environ.get('GITHUB_ACTIONS') == 'true' else ''
    if arch == 'x86_64':
        # glibc's loader lists the x86-64 ISA levels and which this CPU has.
        out = subprocess.run(['/lib64/ld-linux-x86-64.so.2', '--help'],
                             capture_output=True, text=True, check=True).stdout
        print('\n'.join(l for l in out.splitlines() if 'x86-64-v' in l))
        if f'{cpu} (supported' not in out:
            raise SystemExit(f'{prefix}this CPU does not support {cpu}')
    else:
        # glibc has no ISA levels for aarch64; the kernel lists the features.
        features = set()
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('Features'):
                features = set(line.split(':', 1)[1].split())
                break
        print('Features:', ' '.join(sorted(features)))
        missing = [f for f in ARM64_FLOOR_FEATURES if f not in features]
        if missing:
            raise SystemExit(f'{prefix}this CPU lacks {" ".join(missing)}, '
                             f'which {cpu} needs')
    print(f'this CPU supports the {arch} floor {cpu}')


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('step', choices=[n for n, _ in STEPS + RELEASE_STEPS]
                        + ['all', 'release', 'info', 'check-pins', 'check-cpu', 'cpu', 'lanes'])
    parser.add_argument('--compiler', choices=sorted(PINS))
    parser.add_argument('--cc')
    parser.add_argument('--cxx')
    parser.add_argument('--name', help='lane name (build/<name>)')
    parser.add_argument('--arch', choices=sorted(CPU_FLOOR),
                        help='show another architecture (only with --dry-run or info)')
    parser.add_argument('--arch-generic', action='store_true',
                        help='configure -Darch_generic=true (the gcc-generic lane)')
    parser.add_argument('--sanitize', choices=sorted(SANITIZERS),
                        help='a sanitizer lane: address (ASan+UBSan) or thread (TSan)')
    parser.add_argument('--jobs', type=int)
    parser.add_argument('--no-ccache', action='store_true',
                        help='ccache is used when installed (CI: when CCACHE_DIR is set); it does not '
                        'change the generated code')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--event', help='lanes: the GitHub event (pull_request, push)')
    args = parser.parse_args()
    if args.step == 'lanes':
        print(ci_lanes(args.event))
        return 0
    if args.step == 'check-pins':
        check_pins()
        return 0
    if args.step == 'cpu':
        print(cpu_floor(args.arch or host_arch()))
        return 0
    if args.step == 'check-cpu':
        check_cpu(host_arch())
        return 0
    s = Setup(args)
    for note in s.notes:
        print(f'NOTE: {note}', file=sys.stderr)
    if args.step == 'info':
        info(s)
        return 0
    if args.step == 'all' and not args.dry_run:
        s.tree_before = tree_state()
    groups = {'all': STEPS, 'release': RELEASE_STEPS}
    for name, function in STEPS + RELEASE_STEPS:
        if args.step == name or name in [n for n, _ in groups.get(args.step, [])]:
            print(f'== {name}', flush=True)
            function(s)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
