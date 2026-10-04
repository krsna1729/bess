#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Curated clang-tidy gate (M22, D-072).

Runs clang-tidy with the repository's .clang-tidy over the library sources of a
configured clang build directory (tests, benchmarks and fuzzers excluded) and
compares the findings, counted per (file, check), with tools/tidy_baseline.json.
Any count above its baseline fails (new findings); any count below fails too,
so the baseline is lowered in the same change (the list only shrinks). There is
no --update: the baseline is edited by hand and reviewed; --report prints the
current counts in the baseline's format.

Usage:
  tools/check_tidy.py --build-dir build/clang [--jobs 8] [--report] [--files RE]
  tools/check_tidy.py --self-test
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "tidy_baseline.json"
WARNING = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):\d+: (?:warning|error): .* \[(?P<checks>[a-z0-9.,\-]+)\]$")
EXCLUDED = re.compile(r"(_test|_bench|_fuzz)\.cc$|/gtest_main\.cc$|/fuzz/|/protobuf/|\.pb\.cc$")


def library_sources(build_dir, files_re):
    with open(Path(build_dir) / "compile_commands.json") as f:
        entries = json.load(f)
    seen = set()
    for e in entries:
        path = os.path.normpath(os.path.join(e["directory"], e["file"]))
        rel = os.path.relpath(path, ROOT)
        if not rel.startswith("core/") or EXCLUDED.search(rel) or rel in seen:
            continue
        if files_re and not re.search(files_re, rel):
            continue
        seen.add(rel)
        yield rel


def parse(output, build_dir=ROOT):
    """(file relative to the repo, check) -> count, from clang-tidy output.

    clang-tidy prints paths as the compile database names them: relative to
    the build directory (../../core/x.h) or absolute."""
    counts = {}
    seen = set()
    for line in output.splitlines():
        m = WARNING.match(line.strip())
        if not m:
            continue
        key_line = (os.path.normpath(os.path.join(build_dir, m.group("file"))), m.group("line"), m.group("checks"))
        if key_line in seen:  # a header's finding repeats for every source that includes it
            continue
        seen.add(key_line)
        rel = os.path.relpath(os.path.normpath(os.path.join(build_dir, m.group("file"))), ROOT)
        if rel.startswith(".."):
            continue
        for check in m.group("checks").split(","):
            key = (rel, check)
            counts[key] = counts.get(key, 0) + 1
    return counts


def clang_tidy():
    # CI pins clang-tidy-19 (Ubuntu 24.04); the baseline is that version's
    # findings. Another version finds other things: compare with --report.
    for name in (os.environ.get("CLANG_TIDY"), "clang-tidy-19", "clang-tidy"):
        if name and shutil.which(name):
            return name
    raise SystemExit("clang-tidy not found (set CLANG_TIDY)")


def run_tidy(build_dir, source):
    r = subprocess.run([clang_tidy(), "--quiet", "-p", str(build_dir), source],
                       cwd=ROOT, capture_output=True, text=True)
    return r.stdout


def to_json(counts):
    files = {}
    for (f, check), n in sorted(counts.items()):
        files.setdefault(f, {})[check] = n
    return {"_comment": [
        "clang-tidy findings (.clang-tidy profile) that predate the M22 gate (D-072).",
        "tools/check_tidy.py fails on any count above or below an entry, so this list only shrinks.",
        "Edited by hand and reviewed; `tools/check_tidy.py --report` prints the current counts."],
        "files": files}


def compare(current, baseline):
    problems = []
    keys = set(current) | set(baseline)
    for key in sorted(keys):
        cur, base = current.get(key, 0), baseline.get(key, 0)
        if cur > base:
            problems.append(f"  {key[0]}: {key[1]}: {cur} finding(s), baseline {base}: new finding(s); fix them")
        elif cur < base:
            problems.append(f"  {key[0]}: {key[1]}: {cur} finding(s), baseline {base}: the baseline is stale; "
                            "lower it in this change")
    return problems


def load_baseline():
    if not BASELINE.exists():
        return {}
    data = json.loads(BASELINE.read_text())
    return {(f, c): n for f, checks in data.get("files", {}).items() for c, n in checks.items()}


def self_test():
    out = "\n".join([
        f"{ROOT}/core/a.h:3:5: warning: x [bugprone-use-after-move]",
        "../../core/a.h:3:5: warning: x [bugprone-use-after-move]",  # same line, relative path, second TU
        f"{ROOT}/core/b.cc:1:1: warning: y [performance-move-const-arg,bugprone-foo]",
        "/usr/include/x.h:1:1: warning: z [bugprone-bar]",
        "not a warning",
    ])
    counts = parse(out, ROOT / "build" / "x")
    assert counts == {("core/a.h", "bugprone-use-after-move"): 1, ("core/b.cc", "performance-move-const-arg"): 1,
                      ("core/b.cc", "bugprone-foo"): 1}, counts
    assert compare(counts, counts) == []
    assert len(compare(counts, {})) == 3  # growth
    grown = dict(counts)
    grown[("core/a.h", "bugprone-use-after-move")] = 2
    assert "new finding" in compare(grown, counts)[0]
    assert "stale" in compare({}, {("core/a.h", "x"): 1})[0]
    print("Self-test PASSED: parsing, header de-duplication, growth and stale baselines.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--files", help="only sources matching this regex (for local use; the gate runs all)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.build_dir:
        ap.error("--build-dir is required")
    sources = list(library_sources(args.build_dir, args.files))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        output = "\n".join(pool.map(lambda s: run_tidy(args.build_dir, s), sources))
    counts = parse(output, Path(args.build_dir).resolve())
    if args.report:
        print(json.dumps(to_json(counts), indent=2))
        return 0
    if args.files:
        baseline = {k: v for k, v in load_baseline().items() if re.search(args.files, k[0])}
    else:
        baseline = load_baseline()
    problems = compare(counts, baseline)
    if problems:
        print(f"FAILED: clang-tidy findings differ from {BASELINE.name} ({len(problems)} problem(s)):")
        print("\n".join(problems))
        version = subprocess.run([clang_tidy(), "--version"], capture_output=True, text=True).stdout
        print(f"Current counts ({version.strip().splitlines()[-1] if version.strip() else 'clang-tidy'}), "
              "in the baseline's format, for review:")
        print(json.dumps(to_json(counts), indent=2))
        return 1
    total = sum(counts.values())
    print(f"OK: {len(sources)} sources; {total} baselined finding(s) in {len({f for f, _ in counts})} file(s); "
          "no new findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
