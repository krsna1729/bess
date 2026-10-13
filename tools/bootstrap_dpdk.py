#!/usr/bin/env python3
"""Download, verify, patch, build, and install the pinned DPDK dependency.

The local patch series (D-131) is deps/dpdk-patches/NNNN-<slug>.patch, applied
with -p1 in file-name order to the pristine extracted tarball. Its identity (a
sha256 over the ordered names and patch digests) is stamped into the source
tree and into each install prefix; a different stamp re-extracts the source or
reinstalls. `--check-patches` verifies the series without building.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / 'deps' / 'dpdk.json'
PATCH_DIR = ROOT / 'deps' / 'dpdk-patches'
PATCH_NAME = re.compile(r'\d{4}-[A-Za-z0-9][A-Za-z0-9._-]*\.patch')
# Files in PATCH_DIR that are not patches. Anything else is an error, so a
# misnamed patch is never silently left out.
PATCH_DIR_EXTRAS = frozenset({'README.md', '.gitkeep'})
# Written into the source tree and into each install prefix.
STAMP = '.bess-dpdk-patches.json'
TRAILERS = {
    'Upstream-Status': re.compile(
        r'Pending|Submitted https?://\S+|Accepted [0-9a-f]{7,40}|Local-only \S.*'),
    'BESS-Decision': re.compile(r'D-\d{3,}'),
}

# What of DPDK to build. Every library is always built (a plugin may use any of
# them); DPDK's own apps never are. The profiles differ in which drivers:
#   bess  the PCI and vdev buses, the ring/stack mempools and the software and
#         AF_XDP ports BESS and its tests use (null, ring, af_xdp, af_packet,
#         tap). Enough for development and CI.
#   full  every NIC family, for a binary that ships to real hardware. Crypto,
#         event, baseband, regex, ml, compress, vdpa, raw, gpu and dma devices
#         stay off: nothing in BESS uses them.
PROFILES = {
    'bess': [
        '-Ddisable_apps=*',
        '-Denable_drivers=bus/pci,bus/vdev,mempool/ring,mempool/stack,'
        'net/null,net/ring,net/af_xdp,net/af_packet,net/tap',
    ],
    'full': [
        '-Ddisable_apps=*',
        '-Ddisable_drivers=crypto/*,event/*,baseband/*,regex/*,ml/*,'
        'compress/*,vdpa/*,raw/*,gpu/*,dma/*',
    ],
}


def run(command: list[str], *, cwd: Path | None = None,
        env: dict[str, str] | None = None) -> None:
    print('+', ' '.join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def read_metadata() -> dict[str, str]:
    with METADATA.open(encoding='utf-8') as metadata:
        value = json.load(metadata)
    required = {'version', 'directory', 'url', 'sha256'}
    missing = required.difference(value)
    if missing:
        raise SystemExit(f'{METADATA} is missing: {", ".join(sorted(missing))}')
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify_archive(path: Path, expected: str) -> None:
    actual = sha256(path)
    if actual != expected:
        raise SystemExit(
            f'SHA256 mismatch for {path}: expected {expected}, got {actual}')
    print(f'Verified {path.name}: {actual}')


def patch_series(directory: Path = PATCH_DIR) -> list[Path]:
    """The series in application order; [] when the directory is absent or
    holds no patches (pristine DPDK)."""
    if not directory.is_dir():
        return []
    patches, stray = [], []
    for entry in sorted(directory.iterdir()):
        if entry.name in PATCH_DIR_EXTRAS:
            continue
        if entry.is_file() and PATCH_NAME.fullmatch(entry.name):
            patches.append(entry)
        else:
            stray.append(entry.name)
    if stray:
        raise SystemExit(f'{directory}: not a NNNN-<slug>.patch file: '
                         + ', '.join(stray))
    return patches


def series_record(patches: list[Path]) -> dict:
    """The series identity: sha256 over each patch's name and content digest,
    in order. The empty series has the digest of no input."""
    entries = [{'name': p.name, 'sha256': sha256(p)} for p in patches]
    digest = hashlib.sha256()
    for entry in entries:
        digest.update(f'{entry["name"]}\0{entry["sha256"]}\n'.encode())
    return {'series': digest.hexdigest(), 'patches': entries}


EMPTY_SERIES = series_record([])['series']


def read_stamp(path: Path) -> str | None:
    try:
        return json.loads(path.read_text(encoding='utf-8'))['series']
    except (OSError, ValueError, KeyError, TypeError):
        return None


def write_stamp(path: Path, record: dict) -> None:
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + '\n',
                         encoding='utf-8')
    os.replace(temporary, path)


def trailers(patch: Path) -> dict[str, list[str]]:
    """The D-131 trailers in a format-patch commit message (before `---`)."""
    found: dict[str, list[str]] = {key: [] for key in TRAILERS}
    for line in patch.read_text(encoding='utf-8', errors='replace').splitlines():
        if line == '---':
            break
        key, sep, value = line.partition(':')
        if sep and key in found:
            found[key].append(value.strip())
    return found


def patch_problems(patches: list[Path]) -> list[str]:
    """What makes the series invalid without applying it: format, numbering
    and the two trailers."""
    problems = []
    numbers: dict[str, str] = {}
    for patch in patches:
        number = patch.name[:4]
        if number in numbers:
            problems.append(f'{patch.name}: number {number} is also {numbers[number]}')
        numbers[number] = patch.name
        lines = patch.read_text(encoding='utf-8', errors='replace').splitlines()
        if not lines or not lines[0].startswith('From ') or not any(
                line.startswith('Subject: ') for line in lines):
            problems.append(f'{patch.name}: not `git format-patch` output '
                            '(no "From " line or Subject header)')
        for key, values in trailers(patch).items():
            if len(values) != 1:
                problems.append(f'{patch.name}: needs exactly one {key} trailer, '
                                f'has {len(values)}')
            elif not TRAILERS[key].fullmatch(values[0]):
                problems.append(f'{patch.name}: invalid {key}: {values[0]!r}')
    return problems


def fetch_archive(metadata: dict[str, str], archive: Path) -> None:
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        print(f'Downloading {metadata["url"]}')
        temporary = archive.with_suffix(archive.suffix + '.download')
        with urlopen(metadata['url']) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        os.replace(temporary, archive)
    verify_archive(archive, metadata['sha256'])


def extract(metadata: dict[str, str], archive: Path, destination: Path) -> Path:
    with tarfile.open(archive, 'r:*') as package:
        package.extractall(destination, filter='data')
    extracted = destination / metadata['directory']
    if not extracted.is_dir():
        roots = [
            candidate for candidate in destination.iterdir()
            if candidate.is_dir()
        ]
        if len(roots) != 1:
            raise SystemExit(
                f'archive {archive} did not contain a single source directory')
        extracted = roots[0]
    return extracted


def apply_series(source_dir: Path, patches: list[Path]) -> None:
    """Apply the series to a throwaway extraction; the first failure stops
    with the patch's name and its rejected hunks."""
    # git apply inside the BESS checkout would resolve paths against the BESS
    # repository: stop its repository search at the extraction.
    env = {**os.environ, 'GIT_CEILING_DIRECTORIES': str(source_dir.parent)}
    for patch in patches:
        print(f'Applying {patch.name}')
        result = subprocess.run(['git', 'apply', '-p1', '--whitespace=nowarn',
                                 str(patch)], cwd=source_dir, env=env,
                                capture_output=True, text=True)
        if result.returncode == 0:
            continue
        # The tree is discarded: apply what applies to show what does not.
        subprocess.run(['git', 'apply', '-p1', '--whitespace=nowarn', '--reject',
                        str(patch)], cwd=source_dir, env=env, capture_output=True)
        rejects = ''.join(f'--- {rej.relative_to(source_dir)}\n{rej.read_text(errors="replace")}'
                          for rej in sorted(source_dir.rglob('*.rej')))
        raise SystemExit(f'DPDK patch {patch.name} does not apply:\n'
                         f'{result.stderr.strip()}\n{rejects}'.rstrip())


def is_build_tree(name: str) -> bool:
    return name == 'build' or name.startswith('build-')


def is_install_tree(name: str) -> bool:
    return name == 'install' or name.startswith('install-')


def prepare_source(metadata: dict[str, str], source_dir: Path, archive: Path,
                   patches: list[Path], record: dict) -> None:
    """Make source_dir the pristine tarball plus the series. A tree stamped
    with another series (or none) is replaced by a fresh extraction; the
    build and install trees inside it move over. Build trees survive only if
    the old tree was pristine: unchanged files then keep their tarball mtimes
    and patched files are newer, so ninja rebuilds exactly what changed. After
    a patched tree they are removed, since a reverted file goes back to an
    older mtime that ninja would not notice."""
    previous = read_stamp(source_dir / STAMP) if source_dir.exists() else None
    if source_dir.exists() and previous == record['series']:
        print(f'DPDK source at {source_dir} has the current patch series '
              f'({len(patches)} patches)')
        return
    fetch_archive(metadata, archive)
    keep_builds = previous in (None, EMPTY_SERIES)
    with tempfile.TemporaryDirectory(
            prefix='dpdk-extract-', dir=archive.parent) as temporary:
        temporary_path = Path(temporary)
        extracted = extract(metadata, archive, temporary_path / 'new')
        apply_series(extracted, patches)
        write_stamp(extracted / STAMP, record)
        if source_dir.exists():
            print(f'Re-extracting {source_dir}: its patch series '
                  f'{previous or "(no stamp)"} is not {record["series"]}')
            for entry in source_dir.iterdir():
                if is_install_tree(entry.name) or (keep_builds and is_build_tree(entry.name)):
                    os.replace(entry, extracted / entry.name)
            os.replace(source_dir, temporary_path / 'previous')
        os.replace(extracted, source_dir)


def pkg_config_exists(package: str, version: str | None = None) -> bool:
    command = ['pkg-config']
    if version:
        command.append(f'--atleast-version={version}')
    else:
        command.append('--exists')
    command.append(package)
    return subprocess.run(command, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def af_xdp_dependencies_available() -> bool:
    headers = [Path('/usr/include/xdp/xsk.h'), Path('/usr/include/bpf/bpf.h')]
    return (
        pkg_config_exists('libxdp', '1.2.2')
        and pkg_config_exists('libbpf')
        and all(header.exists() for header in headers)
    )


def check_af_xdp_dependencies(mode: str) -> None:
    available = af_xdp_dependencies_available()
    if available:
        print('AF_XDP prerequisites: available')
        return
    message = 'AF_XDP prerequisites are unavailable (libxdp >= 1.2.2, libbpf, headers)'
    if mode == 'required':
        raise SystemExit(message)
    print('Warning:', message, file=sys.stderr)


def check_af_xdp_artifacts(prefix: Path, mode: str) -> None:
    artifacts = [prefix / 'lib' / 'librte_net_af_xdp.so',
                 prefix / 'lib' / 'librte_net_af_xdp.a']
    missing = [str(path) for path in artifacts if not path.is_file()]
    if missing and mode == 'required':
        raise SystemExit('AF_XDP=required but DPDK is missing: ' + ', '.join(missing))
    if missing:
        print('Warning: DPDK AF_XDP PMD was not built', file=sys.stderr)
    else:
        print('AF_XDP PMD artifacts: available')


def machine_options(cpu: str | None, arch: str) -> list[str]:
    """DPDK's machine selection for BESS's `cpu` on `arch` (uname -m).

    x86_64: cpu_instruction_set=<cpu> (DPDK compiles with -march=<cpu>; the
    `machine` spelling is deprecated). aarch64: DPDK ignores
    cpu_instruction_set and picks flags from its SoC table, so `native`
    detects this machine's core and anything else is the generic SoC:
    -march=armv8-a+crc, RTE_CACHE_LINE_SIZE 128, which every armv8.1+ server
    (and so BESS's armv8.2-a floor) runs. BESS's own -march stays its `cpu`.
    """
    if arch == 'aarch64':
        return ['-Dplatform=native' if cpu == 'native' else '-Dplatform=generic']
    if arch == 'x86_64':
        return ['-Dcpu_instruction_set=' + cpu] if cpu else []
    raise SystemExit(f'unsupported architecture {arch}: BESS builds on x86_64 and aarch64')


def setup_command(source_dir: Path, build_dir: Path, prefix: Path,
                  cpu: str | None, profile: str, arch: str) -> list[str]:
    command = ['meson', 'setup']
    if (build_dir / 'meson-private' / 'coredata.dat').exists():
        # A tree configured before the switch from -Dmachine keeps that value,
        # which DPDK refuses next to cpu_instruction_set (and on aarch64 it
        # would override the platform): reset it to its default.
        command.extend(['--reconfigure', '-Dmachine=auto'])
    command.extend([
        str(build_dir),
        str(source_dir),
        '--prefix=' + str(prefix),
        '--libdir=lib',
        '-Dexamples=',
        *PROFILES[profile],
        *machine_options(cpu, arch),
    ])
    return command


def configure_and_build(source_dir: Path, build_dir: Path, prefix: Path,
                         cpu: str | None, profile: str, jobs: int | None,
                         env: dict[str, str], record: dict) -> None:
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    run(setup_command(source_dir, build_dir, prefix, cpu, profile,
                      platform.machine()), env=env)
    ninja_cmd = ['ninja', '-C', str(build_dir)]
    if jobs:
        ninja_cmd.append(f'-j{jobs}')
    run(ninja_cmd, env=env)
    # An install of another series is removed first, so a file only an old
    # patch installed does not linger. No stamp counts as the empty series:
    # every install made before D-131 is pristine.
    installed = read_stamp(prefix / STAMP) or EMPTY_SERIES
    if installed != record['series'] and prefix.exists():
        print(f'Removing {prefix}: installed patch series {installed} '
              f'is not {record["series"]}')
        shutil.rmtree(prefix)
    install_cmd = ['ninja', '-C', str(build_dir)]
    if jobs:
        install_cmd.append(f'-j{jobs}')
    install_cmd.append('install')
    run(install_cmd, env=env)
    write_stamp(prefix / STAMP, record)


def check_patches(metadata: dict[str, str], archive: Path,
                  patches: list[Path]) -> int:
    """Fail unless the series is well formed and applies, in order, to a
    pristine extraction in a temporary directory. Touches no DPDK tree."""
    if not patches:
        print(f'DPDK patches: none in {PATCH_DIR} (pristine DPDK)')
        return 0
    problems = patch_problems(patches)
    if problems:
        raise SystemExit('DPDK patch series is invalid:\n  ' + '\n  '.join(problems))
    fetch_archive(metadata, archive)
    with tempfile.TemporaryDirectory(prefix='dpdk-check-', dir=archive.parent) as temporary:
        apply_series(extract(metadata, archive, Path(temporary)), patches)
    print(f'DPDK patches: {len(patches)} apply cleanly to {metadata["directory"]}; '
          f'series {series_record(patches)["series"]}')
    return 0


def main() -> int:
    metadata = read_metadata()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--af-xdp', choices=('auto', 'required'),
                        default=os.environ.get('AF_XDP', 'auto').lower())
    parser.add_argument('--cpu', default=os.environ.get('CPU'))
    parser.add_argument('--profile', choices=sorted(PROFILES),
                        default=os.environ.get('DPDK_PROFILE', 'full'),
                        help='which DPDK drivers to build: bess (software '
                        'ports only, for development and CI) or full (every '
                        'NIC family, for hardware; default)')
    parser.add_argument('-j', '--jobs', type=int, default=None,
                        help='Number of parallel compile jobs passed to ninja')
    parser.add_argument('--variant', default=os.environ.get('DPDK_VARIANT', ''),
                        help='install into install-<variant> (built in '
                        'build-<variant>) instead of install, so trees that '
                        'need a different compiler, CPU or profile do not '
                        'overwrite each other (tools/ci_profile.py uses one '
                        'per compiler)')
    parser.add_argument('--print-pkg-config-path', action='store_true')
    parser.add_argument('--check-patches', action='store_true',
                        help='check deps/dpdk-patches without building: '
                        'names, both trailers, and that every patch applies '
                        'in order to a pristine extraction (in a temporary '
                        'directory)')
    args = parser.parse_args()

    dpdk_dir = ROOT / 'deps' / metadata['directory']
    suffix = f'-{args.variant}' if args.variant else ''
    prefix = dpdk_dir / f'install{suffix}'
    pkgconfig = prefix / 'lib' / 'pkgconfig'
    if args.print_pkg_config_path:
        print(pkgconfig)
        return 0

    archive = ROOT / 'deps' / f'{metadata["directory"]}.tar.xz'
    patches = patch_series()
    if args.check_patches:
        return check_patches(metadata, archive, patches)
    record = series_record(patches)
    build_dir = dpdk_dir / f'build{suffix}'
    check_af_xdp_dependencies(args.af_xdp)
    prepare_source(metadata, dpdk_dir, archive, patches, record)
    build_env = os.environ.copy()
    build_env['PKG_CONFIG_PATH'] = os.pathsep.join(
        filter(None, [build_env.get('PKG_CONFIG_PATH'), str(pkgconfig)]))
    configure_and_build(dpdk_dir, build_dir, prefix, args.cpu, args.profile,
                        args.jobs, build_env, record)
    print(f'DPDK pkg-config path: {pkgconfig}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
