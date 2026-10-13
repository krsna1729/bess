#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Release artifacts from a release build (M26, D-087, D-135).

    package_release.py tarball --build BUILD --version V [--out DIR]
    package_release.py deb     --build BUILD --version V [--out DIR]

Both come from `meson install` of BUILD, and the runtime has no Python (D-135).

`tarball`: the runtime and the ops tools -- bessd, bessctl (a symlink to
bessd: the C++ client, D-120), the sample manifests, the protobuf schema, the
build metadata and SBOM, and dpdk-devbind.py -- and the docs.

`deb` (on Debian or Ubuntu): five packages -- `bess` (the runtime: bessd,
bessctl, the sample manifests, the protobuf schema, the build metadata and
the SPDX SBOM; no Python), `bess-tools` (dpdk-devbind.py; Python 3),
`bess-python` (the legacy Python client, pybess
and the Python bessctl, with their generated protobuf modules), `bess-dev` (the
headers and bess-dev.pc plugins build against, and the experimental route,
dataplane and RCU archives with bess-dev-static.pc, D-095) and `bess-sdk` (the
C++ control SDK). `bess` depends on the packages that own every shared library
bessd loads (`ldd`, then `dpkg -S`): a library no package owns stops the
build, so the package never claims less than they need.
"""

from __future__ import annotations

import argparse
import json
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


def stage_install(build: str, stage: Path) -> tuple[str, str]:
    """`meson install` of `build` into `stage`; the prefix and libdir, relative."""
    run(['meson', 'install', '-C', build, '--destdir', stage, '--quiet'])
    prefix = build_option(build, 'prefix').strip('/')
    libdir = build_option(build, 'libdir').strip('/')
    lib = libdir if libdir.startswith(prefix + '/') else f'{prefix}/{libdir}'
    bessctl = stage / prefix / 'bin' / 'bessctl'  # the C++ one (G1.7): bessd (D-120)
    if not bessctl.is_symlink() or os.readlink(bessctl) != 'bessd':
        raise SystemExit(f'{bessctl} is not a symlink to bessd')
    return prefix, lib


def runtime_paths(prefix: str) -> list[str]:
    return [f'{prefix}/bin/bessd', f'{prefix}/bin/bessctl', f'{prefix}/share/bess/conf',
            f'{prefix}/share/bess/protobuf', f'{prefix}/share/bess/build-info.json',
            f'{prefix}/share/doc/bess']


def tools_paths(prefix: str) -> list[str]:
    return [f'{prefix}/bin/dpdk-devbind.py']


def python_paths(prefix: str) -> list[str]:
    return [f'{prefix}/share/bess/{tree}' for tree in ('pybess', 'builtin_pb', 'plugin_pb', 'bessctl')]


def copy_paths(stage: Path, dest: Path, paths: list[str], what: str) -> None:
    for rel in paths:
        src = stage / rel
        if not src.exists() and not src.is_symlink():
            raise SystemExit(f'{what}: {rel} is not in the install')
        dst = dest / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.is_symlink():
            dst.symlink_to(os.readlink(src))
        elif src.is_dir():
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)


def check_no_python(root: Path, what: str) -> None:
    found = sorted(str(p.relative_to(root)) for p in root.rglob('*')
                   if p.suffix in ('.py', '.pyc') or p.name == '__pycache__')
    if found:
        raise SystemExit(f'{what} must hold no Python (D-135): ' + ', '.join(found))


def tarball(args) -> Path:
    name = f'bess-{args.version}-linux-{os.uname().machine}'
    out = Path(args.out)
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / 'stage'
        prefix, _ = stage_install(args.build, stage)
        top = Path(tmp) / name
        copy_paths(stage / prefix, top, [p.removeprefix(prefix + '/')
                                         for p in runtime_paths(prefix) + tools_paths(prefix)],
                   'tarball')
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
              out: Path, paths: list[str], python: bool = True,
              takes_over: list[str] | None = None) -> Path:
    with tempfile.TemporaryDirectory() as tmp:
        pkg = Path(tmp) / name
        copy_paths(stage, pkg, paths, name)
        if not python:
            check_no_python(pkg, name)
        size_kb = sum(f.stat().st_size for f in pkg.rglob('*')
                      if f.is_file() and not f.is_symlink()) // 1024
        (pkg / 'DEBIAN').mkdir()
        control = [f'Package: {name}', f'Version: {version}', f'Architecture: {arch()}',
                   'Maintainer: BESS developers <bess@localhost>', f'Installed-Size: {size_kb}',
                   'Section: net', 'Priority: optional', 'Homepage: https://github.com/krsna1729/bess']
        if depends:
            control.append('Depends: ' + ', '.join(depends))
        # Files that move here from an older package (Debian policy 7.6.1).
        if takes_over:
            older = ', '.join(f'{p} (<< {version})' for p in takes_over)
            control += [f'Replaces: {older}', f'Breaks: {older}']
        control.append(f'Description: {description}')
        (pkg / 'DEBIAN' / 'control').write_text('\n'.join(control) + '\n')
        path = out / f'{name}_{version}_{arch()}.deb'
        run(['dpkg-deb', '--root-owner-group', '--build', pkg, path])
    print(path)
    return path


def build_option(build: str, name: str) -> str:
    """A configured build's option (e.g. libdir: lib/x86_64-linux-gnu on Debian)."""
    options = json.loads(subprocess.run(['meson', 'introspect', '--buildoptions', build],
                                        check=True, capture_output=True, text=True).stdout)
    return next(o['value'] for o in options if o['name'] == name)


def deb(args) -> list[Path]:
    out = Path(args.out)
    version = deb_version(args.version)
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / 'stage'
        prefix, lib = stage_install(args.build, stage)
        # Every install under share/bess goes to one of the packages below.
        claimed = {Path(p).name for p in runtime_paths(prefix) + tools_paths(prefix) +
                   python_paths(prefix) if p.startswith(f'{prefix}/share/bess/')}
        unclaimed = sorted({p.name for p in (stage / prefix / 'share' / 'bess').iterdir()} - claimed)
        if unclaimed:
            raise SystemExit('share/bess entries no package claims: ' + ', '.join(unclaimed))
        made = [
            build_deb(stage, 'bess', version, owning_packages(stage / prefix / 'bin' / 'bessd'),
                      'BESS daemon (bessd), bessctl, sample manifests, protobuf schema, build '
                      'metadata and SBOM',
                      out, runtime_paths(prefix), python=False),
            # dpdk-devbind.py runs lspci.
            build_deb(stage, 'bess-tools', version, ['python3', 'pciutils'],
                      'BESS ops tools: dpdk-devbind.py', out,
                      tools_paths(prefix)),
            # grpcio and protobuf at the versions of requirements.txt are newer
            # than the distributions' python3-grpcio and python3-protobuf: pip.
            # Its files were in bess before D-135.
            build_deb(stage, 'bess-python', version, [f'bess (= {version})', 'python3'],
                      'BESS legacy Python client (pybess, Python bessctl); needs grpcio and '
                      'protobuf from pip (requirements.txt)', out, python_paths(prefix),
                      takes_over=['bess']),
            # What bess-dev.pc and bess-dev-static.pc require (glog, gflags,
            # protobuf, libpcap) as packages, so pkg-config resolves on a host
            # that has only these packages.
            build_deb(stage, 'bess-dev', version, [f'bess (= {version})', 'libgoogle-glog-dev',
                                                   'libgflags-dev', 'libprotobuf-dev',
                                                   'libpcap-dev'],
                      'BESS plugin and module development headers (bess-dev.pc)', out,
                      [f'{prefix}/include/bess/core', f'{lib}/pkgconfig/bess-dev.pc',
                       f'{lib}/pkgconfig/bess-dev-static.pc',
                       *(f'{lib}/bess/libbess_{name}.a'
                         for name in ('route', 'dataplane_core', 'eal', 'rcu', 'utils'))]),
            # The control SDK (bess-sdk.pc): a client of any bessd, so it
            # needs no bess package, only what bess-sdk.pc requires.
            build_deb(stage, 'bess-sdk', version, ['libgrpc++-dev', 'libprotobuf-dev'],
                      'BESS C++ control SDK: libbess-sdk, its headers and bess-sdk.pc', out,
                      [f'{prefix}/include/bess/sdk', f'{lib}/pkgconfig/bess-sdk.pc',
                       *(str(p.relative_to(stage))
                         for p in sorted((stage / lib).glob('libbess-sdk*')))]),
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
