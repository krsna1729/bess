#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Release artifacts from a release build (M26, D-087).

    package_release.py tarball --build BUILD --version V [--out DIR]
    package_release.py deb     --build BUILD --version V [--out DIR]

`tarball`: bessd, bessctl, dpdk-devbind.py, the Python client and the docs, as
the release job always shipped them.

`deb` (on Debian or Ubuntu): two packages from `meson install` of BUILD --
`bess` (bessd, the Python client and protobuf schema, the build metadata and
the SPDX SBOM) and `bess-dev` (the headers and bess-dev.pc plugins build
against, and the experimental route, dataplane and RCU archives with
bess-dev-static.pc, D-095). `bess` depends on the packages that own every shared library bessd
loads (`ldd`, then `dpkg -S`): a library no package owns stops the build, so
the package never claims less than bessd needs.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def arch() -> str:
    machine = os.uname().machine
    return {'x86_64': 'amd64', 'aarch64': 'arm64'}.get(machine, machine)


def deb_version(version: str) -> str:
    """A Debian version from a tag (v1.2.3) or a commit (abcdef0): digits first."""
    v = version[1:] if version.startswith('v') else version
    return v if v[:1].isdigit() else f'0.0~git{v}'


def tarball(args) -> Path:
    name = f'bess-{args.version}-linux-{os.uname().machine}'
    out = Path(args.out)
    with tempfile.TemporaryDirectory() as tmp:
        top = Path(tmp) / name
        (top / 'bin').mkdir(parents=True)
        shutil.copy2(Path(args.build) / 'core' / 'bessd', top / 'bin')
        for script in ('bessctl', 'dpdk-devbind.py'):
            shutil.copy2(ROOT / 'bin' / script, top / 'bin')
        for tree in ('bessctl', 'pybess'):
            shutil.copytree(ROOT / tree, top / tree,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        for doc in ('README.md', 'COPYING'):
            if (ROOT / doc).exists():
                shutil.copy2(ROOT / doc, top)
        path = out / f'{name}.tar.gz'
        with tarfile.open(path, 'w:gz') as tar:
            tar.add(top, arcname=name)
    digest = subprocess.run(['sha256sum', path.name], cwd=out, check=True, capture_output=True,
                            text=True).stdout
    (out / f'{name}.tar.gz.sha256').write_text(digest)
    print(path)
    return path


def owning_packages(binary: Path) -> list[str]:
    """The packages that own every shared library `binary` loads."""
    ldd = subprocess.run(['ldd', str(binary)], check=True, capture_output=True, text=True).stdout
    libs = sorted({m.group(1) for m in re.finditer(r'=> (/\S+)', ldd)})
    packages, orphans = set(), []
    for lib in libs:
        real = os.path.realpath(lib)
        found = None
        for candidate in (lib, real, real.replace('/usr/lib', '/lib', 1), lib.replace('/lib', '/usr/lib', 1)):
            r = subprocess.run(['dpkg', '-S', candidate], capture_output=True, text=True)
            if r.returncode == 0:
                found = r.stdout.split(':', 1)[0].split(',')[0].strip()
                break
        if found is None:
            orphans.append(lib)
        else:
            packages.add(found)
    if orphans:
        raise SystemExit('no package owns: ' + ', '.join(orphans) +
                         ' (link it statically or install it from a package)')
    return sorted(packages)


def build_deb(stage: Path, name: str, version: str, depends: list[str], description: str,
              out: Path, paths: list[str]) -> Path:
    with tempfile.TemporaryDirectory() as tmp:
        pkg = Path(tmp) / name
        for rel in paths:
            src = stage / rel
            if not src.exists():
                raise SystemExit(f'{name}: {rel} is not in the install')
            dst = pkg / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if src.is_dir():
                shutil.copytree(src, dst)
            else:
                shutil.copy2(src, dst)
        size_kb = sum(f.stat().st_size for f in pkg.rglob('*') if f.is_file()) // 1024
        (pkg / 'DEBIAN').mkdir()
        control = [f'Package: {name}', f'Version: {version}', f'Architecture: {arch()}',
                   'Maintainer: BESS developers <bess@localhost>', f'Installed-Size: {size_kb}',
                   'Section: net', 'Priority: optional', 'Homepage: https://github.com/krsna1729/bess']
        if depends:
            control.append('Depends: ' + ', '.join(depends))
        control.append(f'Description: {description}')
        (pkg / 'DEBIAN' / 'control').write_text('\n'.join(control) + '\n')
        path = out / f'{name}_{version}_{arch()}.deb'
        run(['dpkg-deb', '--root-owner-group', '--build', pkg, path])
    print(path)
    return path


def deb(args) -> list[Path]:
    out = Path(args.out)
    version = deb_version(args.version)
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / 'stage'
        run(['meson', 'install', '-C', args.build, '--destdir', stage, '--quiet'])
        prefix = 'usr/local'
        bessd = stage / prefix / 'bin' / 'bessd'
        runtime_deps = owning_packages(bessd) + ['python3']
        made = [
            build_deb(stage, 'bess', version, runtime_deps,
                      'BESS daemon (bessd), its Python client and schema, build metadata and SBOM',
                      out, [f'{prefix}/bin/bessd', f'{prefix}/share/bess', f'{prefix}/share/doc/bess']),
            # What bess-dev.pc and bess-dev-static.pc require (glog, gflags,
            # protobuf, libpcap) as packages, so pkg-config resolves on a host
            # that has only these packages.
            build_deb(stage, 'bess-dev', version, [f'bess (= {version})', 'libgoogle-glog-dev',
                                                   'libgflags-dev', 'libprotobuf-dev',
                                                   'libpcap-dev'],
                      'BESS plugin and module development headers (bess-dev.pc)', out,
                      [f'{prefix}/include/bess', f'{prefix}/lib/pkgconfig/bess-dev.pc',
                       f'{prefix}/lib/pkgconfig/bess-dev-static.pc',
                       *(f'{prefix}/lib/bess/libbess_{name}.a'
                         for name in ('route', 'dataplane_core', 'eal', 'rcu', 'utils'))]),
        ]
    return made


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('artifact', choices=('tarball', 'deb'))
    parser.add_argument('--build', required=True, help='a configured, built release build directory')
    parser.add_argument('--version', required=True, help='a tag (v1.2.3) or a commit')
    parser.add_argument('--out', default='.', help='where the artifacts go')
    args = parser.parse_args()
    Path(args.out).mkdir(parents=True, exist_ok=True)
    if args.artifact == 'tarball':
        tarball(args)
    else:
        deb(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
