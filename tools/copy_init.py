#!/usr/bin/env python3
"""Copy a Python package initializer into the Meson build tree."""

import os
import shutil
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} INPUT ACTUAL_OUTPUT MESON_OUTPUT", file=sys.stderr)
        return 2
    source, actual_output, meson_output = sys.argv[1:]
    os.makedirs(os.path.dirname(actual_output), exist_ok=True)
    os.makedirs(os.path.dirname(meson_output), exist_ok=True)
    shutil.copyfile(source, actual_output)
    shutil.copyfile(actual_output, meson_output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
