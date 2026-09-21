#!/usr/bin/env python3
"""Download, verify, build, and install the pinned DPDK dependency."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / 'deps' / 'dpdk.json'


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
    with tempfile.TemporaryDirectory(prefix='dpdk-extract-', dir=archive.parent) as temporary:
        temporary_path = Path(temporary)
        with tarfile.open(archive, 'r:*') as package:
            package.extractall(temporary_path, filter='data')
        extracted = temporary_path / metadata['directory']
        if not extracted.is_dir():
            raise SystemExit(
                f'archive {archive} did not contain {metadata["directory"]}/')
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


def configure_and_build(source_dir: Path, build_dir: Path, prefix: Path,
                         cpu: str | None, env: dict[str, str]) -> None:
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    command = ['meson', 'setup']
    if (build_dir / 'meson-private' / 'coredata.dat').exists():
        command.append('--reconfigure')
    command.extend([
        str(build_dir),
        str(source_dir),
        '--prefix=' + str(prefix),
        '--libdir=lib',
        '-Dexamples=',
    ])
    if cpu:
        command.append('-Dmachine=' + cpu)
    run(command, env=env)
    run(['ninja', '-C', str(build_dir)], env=env)
    run(['ninja', '-C', str(build_dir), 'install'], env=env)


def main() -> int:
    metadata = read_metadata()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--af-xdp', choices=('auto', 'required'),
                        default=os.environ.get('AF_XDP', 'auto').lower())
    parser.add_argument('--cpu', default=os.environ.get('CPU'))
    parser.add_argument('--print-pkg-config-path', action='store_true')
    args = parser.parse_args()

    dpdk_dir = ROOT / 'deps' / metadata['directory']
    prefix = dpdk_dir / 'install'
    pkgconfig = prefix / 'lib' / 'pkgconfig'
    if args.print_pkg_config_path:
        print(pkgconfig)
        return 0

    archive = ROOT / 'deps' / f'{metadata["directory"]}.tar.xz'
    build_dir = dpdk_dir / 'build'
    check_af_xdp_dependencies(args.af_xdp)
    download_and_extract(metadata, dpdk_dir, archive)
    build_env = os.environ.copy()
    build_env['PKG_CONFIG_PATH'] = os.pathsep.join(
        filter(None, [build_env.get('PKG_CONFIG_PATH'), str(pkgconfig)]))
    configure_and_build(dpdk_dir, build_dir, prefix, args.cpu, build_env)
    check_af_xdp_artifacts(prefix, args.af_xdp)
    print(f'DPDK pkg-config path: {pkgconfig}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
