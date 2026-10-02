#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Which protoc-generated headers the public C++ headers need (M2, D-058).

The installed `bess-dev` headers include a few generated `pb/*.pb.h` (message.h
includes pb/bess_msg.pb.h, ...), and those include other generated headers
(bess_msg.pb.h includes error.pb.h). Installing the whole generated directory
would also ship `*.grpc.pb.h`, the control and test protocols and the `.pb.cc`
sources, so the install names exactly the closure instead. This script computes
that closure from the real include graph, so it is also the check that the
named list is neither short nor long.

  public_proto_closure.py --core-dir core --pb-dir BUILD/protobuf/generated/cpp/core/pb
      the closure for the headers named in tools/api_classes.json, one per line
  public_proto_closure.py ... --check
      exit 1 unless it equals the "generated" list of tools/api_classes.json

--core-dir is the header root: the source `core/` directory, or the installed
`include/bess/core`. --pb-dir is where the generated headers live (the build's
generated directory, or the installed `pb/`).
"""

import argparse
import json
from pathlib import Path
import posixpath
import re
import sys

MANIFEST = Path(__file__).resolve().with_name("api_classes.json")
QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def quoted_includes(path):
    return QUOTED_INCLUDE.findall(path.read_text(encoding="utf-8", errors="ignore"))


def closure(core_dir, headers, pb_dir):
    """Generated headers (as `pb/x.pb.h`) reachable from `headers`.

    Walks quoted includes through the given headers (a header root-relative
    path each), then through the generated headers themselves, whose includes
    of each other are spelled relative to their own directory (`"error.pb.h"`).
    A generated header that does not exist in `pb_dir` is an error: the graph
    names something the build does not produce.
    """
    core_dir, pb_dir = Path(core_dir), Path(pb_dir)
    wanted = set()
    for header in headers:
        for inc in quoted_includes(core_dir / header):
            target = posixpath.normpath(inc)
            if target.startswith("pb/") and target.endswith(".pb.h"):
                wanted.add(target)
    pending = sorted(wanted)
    while pending:
        target = pending.pop()
        generated = pb_dir / posixpath.basename(target)
        if not generated.is_file():
            raise SystemExit(f"{target} is included but {generated} does not exist")
        for inc in quoted_includes(generated):
            sibling = posixpath.normpath(posixpath.join("pb", inc))
            if sibling.endswith(".pb.h") and (pb_dir / posixpath.basename(sibling)).is_file():
                if sibling not in wanted:
                    wanted.add(sibling)
                    pending.append(sibling)
    return wanted


def load_manifest(path=MANIFEST):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return data["headers"], data["generated"]["headers"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--core-dir", required=True, type=Path)
    parser.add_argument("--pb-dir", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--check", action="store_true",
                        help="fail unless the closure equals the manifest's generated list")
    args = parser.parse_args()

    headers, generated = load_manifest(args.manifest)
    found = closure(args.core_dir, headers, args.pb_dir)
    if not args.check:
        print("\n".join(sorted(found)))
        return 0
    listed = set(generated)
    if found != listed:
        for name in sorted(found - listed):
            print(f"needed by a public header but not in the manifest: {name}", file=sys.stderr)
        for name in sorted(listed - found):
            print(f"in the manifest but no public header needs it: {name}", file=sys.stderr)
        return 1
    print(f"OK: the public headers need exactly {len(found)} generated headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
