#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Regenerates the committed fuzz seed corpora (core/fuzz/corpus/<target>/).

Each target's seeds come from core/fuzz/seeds/<target>.py, whose seeds()
returns {file name: bytes} encoded in that harness's input format. Output is
deterministic: the same script writes the same files. Stale files in a
target's corpus directory are removed.

  make_seeds.py            rewrite every corpus
  make_seeds.py nat tunnel_decap
  make_seeds.py --check    exit 1 if a committed corpus differs
"""

import argparse
import importlib.util
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
SEEDS = HERE / "seeds"
CORPUS = HERE / "corpus"


def load(target):
    spec = importlib.util.spec_from_file_location(
        f"bess_fuzz_seeds_{target}", SEEDS / f"{target}.py")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(SEEDS))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module.seeds()


def targets():
    return sorted(p.stem for p in SEEDS.glob("*.py") if not p.stem.startswith("_"))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("targets", nargs="*", help="targets (default: all)")
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed corpus instead of writing")
    args = parser.parse_args()

    names = args.targets or targets()
    stale = []
    for target in names:
        seeds = load(target)
        if not seeds:
            sys.exit(f"{target}: no seeds")
        out = CORPUS / target
        existing = {p.name: p.read_bytes() for p in out.glob("*")} if out.is_dir() else {}
        if args.check:
            if existing != seeds:
                stale.append(target)
            continue
        out.mkdir(parents=True, exist_ok=True)
        for name in existing.keys() - seeds.keys():
            (out / name).unlink()
        for name, data in sorted(seeds.items()):
            if existing.get(name) != data:
                (out / name).write_bytes(data)
        print(f"{target}: {len(seeds)} seeds")
    if stale:
        sys.exit("stale corpus (run core/fuzz/make_seeds.py): " + ", ".join(stale))


if __name__ == "__main__":
    main()
