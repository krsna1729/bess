#!/usr/bin/env python3
"""Smoke-test the installed pybess client and generated protobuf packages."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


SMOKE = r'''
import importlib

from pybess import bess
builtin = importlib.import_module('builtin_pb.bess_msg_pb2')
assert hasattr(builtin, 'VersionResponse'), dir(builtin)
port = importlib.import_module('builtin_pb.ports.port_msg_pb2')
plugin = importlib.import_module('plugin_pb.supdate_msg_pb2')
assert hasattr(port, 'PMDPortArg'), dir(port)
assert hasattr(plugin, 'SequentialUpdateArg'), dir(plugin)
assert hasattr(bess.port_msg, 'PMDPortArg'), dir(bess.port_msg)

'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True,
                        help='installed share/bess directory')
    args = parser.parse_args()

    root = Path(args.root).resolve()
    required = [
        root / 'pybess' / '__init__.py',
        root / 'builtin_pb' / '__init__.py',
        root / 'builtin_pb' / 'ports' / 'port_msg_pb2.py',
        root / 'plugin_pb' / 'supdate_msg_pb2.py',
    ]
    missing = [path for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError(', '.join(map(str, missing)))

    environment = os.environ.copy()
    environment['BESS_PROTOBUF_ROOT'] = str(root)
    environment['PYTHONPATH'] = str(root)
    environment['PYTHONDONTWRITEBYTECODE'] = '1'
    subprocess.run(
        [sys.executable, '-c', SMOKE],
        cwd='/',
        env=environment,
        check=True,
    )
    print(f'Installed pybess smoke passed: {root}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
