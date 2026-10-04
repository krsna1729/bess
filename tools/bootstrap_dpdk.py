#!/usr/bin/env python3
"""Download, verify, build, and install the pinned DPDK dependency."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / 'deps' / 'dpdk.json'

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


def download_and_extract(metadata: dict[str, str], source_dir: Path,
                         archive: Path) -> None:
    source_dir.parent.mkdir(parents=True, exist_ok=True)
    if source_dir.exists():
        print(f'DPDK source already exists at {source_dir}')
        return

    if not archive.exists():
        print(f'Downloading {metadata["url"]}')
        temporary = archive.with_suffix(archive.suffix + '.download')
        with urlopen(metadata['url']) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        os.replace(temporary, archive)

    verify_archive(archive, metadata['sha256'])
    with tempfile.TemporaryDirectory(
            prefix='dpdk-extract-', dir=archive.parent) as temporary:
        temporary_path = Path(temporary)
        with tarfile.open(archive, 'r:*') as package:
            package.extractall(temporary_path, filter='data')
        extracted = temporary_path / metadata['directory']
        if not extracted.is_dir():
            roots = [
                candidate for candidate in temporary_path.iterdir()
                if candidate.is_dir()
            ]
            if len(roots) != 1:
                raise SystemExit(
                    f'archive {archive} did not contain a single source directory')
            extracted = roots[0]
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
                         env: dict[str, str]) -> None:
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    run(setup_command(source_dir, build_dir, prefix, cpu, profile,
                      platform.machine()), env=env)
    ninja_cmd = ['ninja', '-C', str(build_dir)]
    if jobs:
        ninja_cmd.append(f'-j{jobs}')
    run(ninja_cmd, env=env)
    install_cmd = ['ninja', '-C', str(build_dir)]
    if jobs:
        install_cmd.append(f'-j{jobs}')
    install_cmd.append('install')
    run(install_cmd, env=env)


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
    args = parser.parse_args()

    dpdk_dir = ROOT / 'deps' / metadata['directory']
    suffix = f'-{args.variant}' if args.variant else ''
    prefix = dpdk_dir / f'install{suffix}'
    pkgconfig = prefix / 'lib' / 'pkgconfig'
    if args.print_pkg_config_path:
        print(pkgconfig)
        return 0

    archive = ROOT / 'deps' / f'{metadata["directory"]}.tar.xz'
    build_dir = dpdk_dir / f'build{suffix}'
    check_af_xdp_dependencies(args.af_xdp)
    download_and_extract(metadata, dpdk_dir, archive)
    build_env = os.environ.copy()
    build_env['PKG_CONFIG_PATH'] = os.pathsep.join(
        filter(None, [build_env.get('PKG_CONFIG_PATH'), str(pkgconfig)]))
    configure_and_build(dpdk_dir, build_dir, prefix, args.cpu, args.profile,
                        args.jobs, build_env)
    check_af_xdp_artifacts(prefix, args.af_xdp)
    print(f'DPDK pkg-config path: {pkgconfig}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
