#!/usr/bin/env python3
"""Write the current Git description without touching unchanged output."""

import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2

    output = sys.argv[1]
    try:
        version = subprocess.check_output(
            ['git', 'describe', '--dirty', '--always', '--tags'],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        version = 'unknown'

    content = f'#pragma once\n#define VERSION "{version}"\n'
    try:
        with open(output, encoding='utf-8') as existing:
            if existing.read() == content:
                return 0
    except FileNotFoundError:
        pass

    temporary = output + '.tmp'
    with open(temporary, 'w', encoding='utf-8') as generated:
        generated.write(content)
    os.replace(temporary, output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
