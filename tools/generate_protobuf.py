#!/usr/bin/env python3
"""Generate protobuf sources in a build directory and publish Meson outputs."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 9:
        print(
            f'usage: {sys.argv[0]} PROTOC PLUGIN MODE PROTO_ROOT GENERATED_ROOT '
            'INPUT OUTPUT...',
            file=sys.stderr,
        )
        return 2

    protoc, plugin, mode, proto_root, generated_root = sys.argv[1:6]
    input_path = Path(sys.argv[6]).resolve()
    outputs = [Path(path).resolve() for path in sys.argv[7:]]
    proto_root_path = Path(proto_root).resolve()
    generated_root_path = Path(generated_root).resolve()

    try:
        relative_proto = input_path.relative_to(proto_root_path)
    except ValueError:
        print(f'{input_path} is outside protobuf root {proto_root_path}', file=sys.stderr)
        return 2

    generated_root_path.mkdir(parents=True, exist_ok=True)
    command = [
        protoc,
        f'--proto_path={proto_root_path}',
        f'--plugin=protoc-gen-grpc={plugin}',
        f'--{mode}_out={generated_root_path}',
        f'--grpc_out={generated_root_path}',
        str(input_path),
    ]
    subprocess.run(command, check=True)

    stem = relative_proto.with_suffix('')
    if mode == 'cpp':
        generated = [
            generated_root_path / stem.parent / f'{stem.name}.pb.cc',
            generated_root_path / stem.parent / f'{stem.name}.pb.h',
            generated_root_path / stem.parent / f'{stem.name}.grpc.pb.cc',
            generated_root_path / stem.parent / f'{stem.name}.grpc.pb.h',
        ]
    elif mode == 'python':
        generated = [
            generated_root_path / stem.parent / f'{stem.name}_pb2.py',
            generated_root_path / stem.parent / f'{stem.name}_pb2_grpc.py',
        ]
    else:
        print(f'unsupported protobuf mode: {mode}', file=sys.stderr)
        return 2

    if len(generated) != len(outputs):
        print('protobuf output count mismatch', file=sys.stderr)
        return 2

    for source, destination in zip(generated, outputs):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
