#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build and verify BESS exactly the way the gating CI jobs do.

CI (.github/workflows/ci.yml) and a developer's machine run this one script, so
the configuration cannot drift: the Meson options, the DPDK build (profile,
CPU floor, AF_XDP, the compiler that builds it) and the verification steps are
defined here and nowhere else.

    tools/ci_profile.py all --compiler gcc       # what the "gcc" lane does
    tools/ci_profile.py all --compiler clang     # what the "clang" lane does
    tools/ci_profile.py info --compiler clang    # resolved setup and divergences
    tools/ci_profile.py build --compiler gcc     # one step (they are re-runnable)

Steps, in CI order: bootstrap (DPDK), configure, build, verify-dpdk, layers,
test, verify-install. `all` runs them in that order.

What this cannot reproduce on a developer machine, and `info` says so:
  * the exact compiler versions, when gcc-14 / clang-19 are not installed (the
    nearest installed compiler is used and flagged);
  * the GitHub runner's 512 reserved hugepages (tests here use the no-hugepage
    mode);
  * the runner's CPU (the floor is x86-64-v3; the code is built for it, but a
    CPU that lacks it cannot run the result).

Local runs never exceed 8 build jobs (docs: MODERNIZATION.md "Build profiles").
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The CI lanes. These must equal the matrix in .github/workflows/ci.yml;
# `check-pins` fails when they do not, and CI runs it.
PINS = {
    'gcc': ('gcc-14', 'g++-14'),
    'clang': ('clang-19', 'clang++-19'),
}
FALLBACK = {
    'gcc': ('gcc', 'g++'),
    'clang': ('clang', 'clang++'),
}

CPU = 'x86-64-v3'  # D-016: the portable floor
DPDK_PROFILE = 'bess'
MESON_OPTIONS = [
    f'-Dcpu={CPU}',
    '-Daf_xdp=required',
    '-Dbuildtype=debugoptimized',
    '-Dbuild_sample_plugin=true',
    # build_benchmarks stays at its default (true): CI compiles the benchmarks,
    # so every other build of this profile must as well.
]
STANDALONE_PLUGINS = ('standalone_pass', 'standalone_macswap',
                      'standalone_range_gate')
LOCAL_JOB_CAP = 8


class Setup:
    def __init__(self, args):
        self.dry_run = args.dry_run
        self.in_ci = os.environ.get('GITHUB_ACTIONS') == 'true'
        self.notes = []
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
            self.name = args.name or f'ci-{family}'
        self.build_dir = ROOT / 'build' / self.name
        self.stage = ROOT / 'build' / f'stage-{self.name}'
        self.standalone = ROOT / 'build' / f'standalone-{self.name}'
        self.variant = self.name
        jobs = args.jobs or (4 if self.in_ci else min(LOCAL_JOB_CAP, os.cpu_count() or 1))
        if not self.in_ci and jobs > LOCAL_JOB_CAP:
            raise SystemExit(f'refusing --jobs {jobs}: local builds are capped at '
                             f'{LOCAL_JOB_CAP}')
        self.jobs = jobs
        launcher = ''
        if not self.in_ci and not args.no_ccache and shutil.which('ccache'):
            launcher = 'ccache '
        self.env = os.environ.copy()
        self.env['CC'] = launcher + self.cc
        self.env['CXX'] = launcher + self.cxx
        self.env['CPU'] = CPU
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

    def env_with_dpdk(self, *extra):
        env = self.env.copy()
        paths = [*extra, self.dpdk_pkgconfig()]
        if env.get('PKG_CONFIG_PATH'):
            paths.append(env['PKG_CONFIG_PATH'])
        env['PKG_CONFIG_PATH'] = os.pathsep.join(paths)
        return env


def step_bootstrap(s):
    s.run([sys.executable, ROOT / 'tools' / 'bootstrap_dpdk.py',
           '--af-xdp', 'required', '--cpu', CPU, '--profile', DPDK_PROFILE,
           '--variant', s.variant, '-j', s.jobs])


def step_configure(s):
    command = ['meson', 'setup']
    if (s.build_dir / 'meson-private' / 'coredata.dat').exists():
        command.append('--reconfigure')
    command += [s.build_dir, *MESON_OPTIONS]
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


def step_test(s):
    s.run(['meson', 'test', '-C', s.build_dir, '--no-rebuild', '--print-errorlogs'],
          env=s.env_with_dpdk())


def step_verify_install(s):
    env = s.env_with_dpdk()
    s.run(['meson', 'install', '-C', s.build_dir, '--destdir', s.stage], env=env)
    if s.dry_run:
        print('+ check installed headers, build standalone plugins, check bessd')
        return
    pc = next(s.stage.rglob('bess-dev.pc'), None)
    if pc is None:
        raise SystemExit('bess-dev.pc was not installed')
    env['PKG_CONFIG_PATH'] = os.pathsep.join([str(pc.parent), env['PKG_CONFIG_PATH']])
    env['CXX'] = s.env['CXX']
    include = s.stage / 'usr/local/include/bess'
    env['CXXFLAGS'] = f'-I{include} -I{include}/core ' + env.get('CXXFLAGS', '')
    s.run([sys.executable, ROOT / 'tools' / 'check_installed_headers.py',
           '--include-dir', include / 'core'], env=env)
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


STEPS = [
    ('bootstrap', step_bootstrap),
    ('configure', step_configure),
    ('build', step_build),
    ('verify-dpdk', step_verify_dpdk),
    ('layers', step_layers),
    ('test', step_test),
    ('verify-install', step_verify_install),
]

# The release job (publish-release in ci.yml): a static standalone bessd built
# with the same compiler as the gating gcc lane, the `full` DPDK profile, and
# no benchmarks or sample plugin. It shares this script's compiler and CPU
# authority; only the options below are release-specific. Not part of `all`.
RELEASE_MESON_OPTIONS = [
    f'-Dcpu={CPU}',
    '-Dbuildtype=release',
    '-Dstatic_binary=standalone',
    '-Daf_xdp=required',
    '-Dbuild_benchmarks=false',
    '-Dbuild_sample_plugin=false',
]


def step_release_bootstrap(s):
    s.run([sys.executable, ROOT / 'tools' / 'bootstrap_dpdk.py',
           '--af-xdp', 'required', '--cpu', CPU, '--profile', 'full',
           '--variant', s.variant, '-j', s.jobs])


def step_release_configure(s):
    command = ['meson', 'setup']
    if (s.build_dir / 'meson-private' / 'coredata.dat').exists():
        command.append('--reconfigure')
    command += [s.build_dir, *RELEASE_MESON_OPTIONS]
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
    print(f'build dir       {s.build_dir}')
    print(f'DPDK            profile {DPDK_PROFILE}, cpu {CPU}, variant {s.variant}')
    print(f'meson options   {" ".join(MESON_OPTIONS)} (benchmarks on, default)')
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


def check_pins():
    import yaml
    workflow = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text())
    lanes = {m['name']: (m['cc'], m['cxx'])
             for m in workflow['jobs']['build-and-test']['strategy']['matrix']['include']
             if not m.get('experimental')}
    bad = []
    for family, pair in PINS.items():
        if lanes.get(family) != pair:
            bad.append(f'{family}: script pins {pair}, ci.yml has {lanes.get(family)}')
    extra = set(lanes) - set(PINS)
    if extra:
        bad.append(f'ci.yml gating lanes without a pin here: {sorted(extra)}')
    if bad:
        raise SystemExit('ci_profile.py and ci.yml disagree:\n  ' + '\n  '.join(bad))
    print('pins match ci.yml gating lanes:', ', '.join(sorted(lanes)))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('step', choices=[n for n, _ in STEPS + RELEASE_STEPS]
                        + ['all', 'release', 'info', 'check-pins'])
    parser.add_argument('--compiler', choices=sorted(PINS))
    parser.add_argument('--cc')
    parser.add_argument('--cxx')
    parser.add_argument('--name', help='lane name (build/<name>)')
    parser.add_argument('--jobs', type=int)
    parser.add_argument('--no-ccache', action='store_true',
                        help='local runs use ccache when installed; it does not '
                        'change the generated code')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if args.step == 'check-pins':
        check_pins()
        return 0
    s = Setup(args)
    for note in s.notes:
        print(f'NOTE: {note}', file=sys.stderr)
    if args.step == 'info':
        info(s)
        return 0
    groups = {'all': STEPS, 'release': RELEASE_STEPS}
    for name, function in STEPS + RELEASE_STEPS:
        if args.step == name or name in [n for n, _ in groups.get(args.step, [])]:
            print(f'== {name}', flush=True)
            function(s)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
