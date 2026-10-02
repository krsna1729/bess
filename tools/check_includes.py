#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS Layer Dependency Include Checker.

Enforces the BESS target architecture DAG (docs/architecture.md, Milestone M1)
by verifying that lower-level libraries do not include higher-level components
(e.g., pure libraries must not depend on Module, runtime state, or control RPCs).

Usage:
  tools/check_includes.py [--root DIR] [--self-test] [--verbose]
  tools/check_includes.py --emit-dotdot-baseline   # regenerate the grandfather list

Each quoted include is resolved the way the compiler does (relative to the
including file, then the `core/` include root) and the rules are applied to the
resolved path as well as to the spelling, so `#include "../runtime/x.h"` is the
edge `runtime/x.h`. `..` components are banned outright, except for the
grandfathered (file, include) pairs in include_dotdot_baseline.txt.
"""

import argparse
from pathlib import Path
import posixpath
import re
import sys

INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s+(["<])([^">]+)[">]')

# Every `..` include that predates the ban, as (file, include) pairs. M1: a new
# `..` include is refused; an entry whose include is gone must be deleted, so the
# list only shrinks. Owner: M1 follow-up (include hygiene). Removal: rewrite the
# include as root-relative (`"../utils/x.h"` -> `"utils/x.h"`, the same file since
# core/ is the include root), a no-codegen mechanical pass best done when no
# other branch is editing core/modules, core/drivers and the hook libraries.
DOTDOT_BASELINE = Path(__file__).resolve().with_name("include_dotdot_baseline.txt")

# Forbidden dependency rules: (source_path_pattern, list_of_forbidden_include_patterns)
FORBIDDEN_RULES = [
    # Packet substrate must be independent of framework, runtime, and control
    (
        re.compile(r"^core/packet/|^core/packet\.[ch]"),
        [
            ("framework/", "packet substrate must not depend on framework"),
            ("runtime/", "packet substrate must not depend on runtime"),
            ("control/", "packet substrate must not depend on control"),
            ("pb/", "packet substrate must not depend on protobuf"),
            ("module.h", "packet substrate must not depend on Module"),
        ],
    ),
    # Dataplane core substrate must not depend on framework, runtime, or control,
    # nor on the batteries built on top of it (meter, route, classifier, stats):
    # the edge is batteries -> substrate, never back (M8, D-050). Tests and
    # benchmarks are exempt.
    (
        re.compile(r"^core/dataplane/"),
        [
            ("framework/", "dataplane core must not depend on framework"),
            ("runtime/", "dataplane core must not depend on runtime"),
            ("control/", "dataplane core must not depend on control"),
            ("pb/", "dataplane core must not depend on protobuf"),
            ("google/protobuf/", "dataplane core must not depend on protobuf"),
            ("grpc", "dataplane core must not depend on gRPC"),
            ("module.h", "dataplane core must not depend on Module"),
            ("meter/", "dataplane core must not depend on the meter battery"),
            ("route/", "dataplane core must not depend on the route library"),
            ("classifier/", "dataplane core must not depend on the classifier battery"),
            ("stats/", "dataplane core must not depend on the stats battery"),
            ("flow/", "dataplane core must not depend on flow: the edge is flow -> dataplane"),
            ("gate.h", "dataplane core must not depend on gates"),
            ("worker.h", "dataplane core must not include worker.h; pass ticks and tokens in"),
        ],
    ),
    # The handoff channel and the continuation table (M11, D-054) move an
    # opaque PacketHandle (an rte_mbuf pointer) and never look inside a packet,
    # so they sit below the packet view: packet_handle.h only. The generic
    # dataplane rule above covers them too. Tests and benchmarks are exempt.
    (
        re.compile(r"^core/dataplane/(handoff|continuation)"),
        [
            ("packet.h", "handoff/continuation must not include the packet view; packet_handle.h is enough"),
            ("pktbatch.h", "handoff/continuation must not depend on PacketBatch"),
            ("packet_pool.h", "handoff/continuation must not depend on the packet pool"),
        ],
    ),
    # Reusable classifier battery must not depend on runtime, control, or Module
    (
        re.compile(r"^core/classifier/"),
        [
            ("runtime/", "classifier battery must not depend on runtime"),
            ("control/", "classifier battery must not depend on control"),
            ("module.h", "classifier battery must not depend on Module"),
        ],
    ),
    # Reusable meter battery must not depend on Module, runtime, or control
    (
        re.compile(r"^core/meter/"),
        [
            ("module.h", "meter battery must not depend on Module"),
            ("runtime/", "meter battery must not depend on runtime"),
            ("control/", "meter battery must not depend on control"),
        ],
    ),
    # Reusable stats battery must not depend on Module, runtime, or control
    (
        re.compile(r"^core/stats/"),
        [
            ("module.h", "stats battery must not depend on Module"),
            ("runtime/", "stats battery must not depend on runtime"),
            ("control/", "stats battery must not depend on control"),
        ],
    ),
    # The route library (table and Router) is graph-independent: no Module, gate,
    # runtime, control or protobuf (M7, D-049). Tests and benchmarks are exempt.
    (
        re.compile(r"^core/route/"),
        [
            ("module.h", "route library must not depend on Module"),
            ("gate.h", "route library must not depend on gates; use InterfaceId"),
            ("runtime/", "route library must not depend on runtime"),
            ("control/", "route library must not depend on control"),
            ("pb/", "route library must not depend on protobuf"),
        ],
    ),
    # The flow-state library is graph-independent and sits below the worker: no
    # Module, gate, framework, runtime, control or protobuf, and no worker.h or
    # stats/current_worker.h (worker identity is an injected owner token; M9,
    # D-052). Tests and benchmarks are exempt.
    (
        re.compile(r"^core/flow/"),
        [
            ("module.h", "flow library must not depend on Module"),
            ("gate.h", "flow library must not depend on gates"),
            ("framework/", "flow library must not depend on framework"),
            ("runtime/", "flow library must not depend on runtime"),
            ("control/", "flow library must not depend on control"),
            ("pb/", "flow library must not depend on protobuf"),
            ("google/protobuf/", "flow library must not depend on protobuf"),
            ("grpc", "flow library must not depend on gRPC"),
            ("worker.h", "flow library must not include worker.h; inject an owner token"),
        ],
    ),
    # Modules obtain runtime facilities through Module::init_context() (D-042),
    # never by including the runtime state. Tests and benchmarks are exempt.
    (
        re.compile(r"^core/modules/"),
        [
            ("runtime/", "modules must use Module::init_context(), not runtime"),
        ],
    ),
]


def resolve_include(rel_path, quote, inc, exists):
    """The `core/`-relative path an include names, resolved as the compiler does.

    A quoted include is looked up next to the including file first, then under
    the include root (core/); a `<>` include only under the root. `exists(p)`
    says whether repo-relative path `p` is a file. An include that resolves to
    nothing (a generated pb/ header, a system header) is judged by its
    normalised spelling under the root. The rules see this path, not the
    spelling: `"runtime/../utils/x.h"` is utils/x.h and `"../runtime/x.h"` is
    runtime/x.h.
    """
    root_form = posixpath.normpath(posixpath.join("core", inc))
    candidates = []
    if quote == '"':
        candidates.append(
            posixpath.normpath(posixpath.join(posixpath.dirname(rel_path), inc))
        )
    candidates.append(root_form)
    resolved = next((c for c in candidates if exists(c)), root_form)
    return resolved[len("core/"):] if resolved.startswith("core/") else resolved


def has_dotdot(inc):
    return ".." in inc.split("/")


def judge_include(rel_path, quote, inc, exists, dotdot_allowed=frozenset(),
                  layering=True, rules=None):
    """Return (reason, ...) for every rule the include breaks."""
    reasons = []
    if has_dotdot(inc) and (rel_path, inc) not in dotdot_allowed:
        reasons.append(
            "'..' in an include path is banned: spell it relative to the "
            "include root (core/)"
        )
    if layering:
        target = resolve_include(rel_path, quote, inc, exists)
        for file_re, file_rules in FORBIDDEN_RULES if rules is None else rules:
            if file_re.search(rel_path):
                for forbidden_sub, reason in file_rules:
                    if forbidden_sub in target:
                        reasons.append(reason)
                        break
    return reasons


def is_fixture(rel_path):
    # Unit tests and microbenchmarks are allowed integration fixtures
    return "_test.cc" in rel_path or "_bench.cc" in rel_path or "gtest_main.cc" in rel_path


def scan_lines(rel_path, lines, exists, dotdot_allowed=frozenset(), rules=None):
    """Violations (path, line, include, reason) of one file's lines."""
    violations = []
    layering = not is_fixture(rel_path)
    for line_no, line in enumerate(lines, 1):
        m = INCLUDE_PATTERN.match(line)
        if not m:
            continue
        inc = m.group(2).replace("\\", "/")
        for reason in judge_include(
            rel_path, m.group(1), inc, exists, dotdot_allowed, layering, rules
        ):
            violations.append((rel_path, line_no, inc, reason))
    return violations


def load_dotdot_baseline(path=DOTDOT_BASELINE):
    pairs = set()
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if raw.strip() and not raw.startswith("#"):
            file, inc = raw.split("\t")
            pairs.add((file, inc))
    return pairs


def core_sources(root):
    for p in sorted(root.glob("core/**/*.[ch]*")):
        yield p, str(p.relative_to(root)).replace("\\", "/")


def stale_baseline(baseline, seen):
    return [
        (file, 0, inc, "stale entry in include_dotdot_baseline.txt: the include "
         "is gone, delete the line")
        for file, inc in sorted(baseline - seen)
    ]


def check_includes(root_dir, verbose=False, baseline_path=DOTDOT_BASELINE):
    root = Path(root_dir)
    baseline = load_dotdot_baseline(baseline_path)
    violations = []
    seen_dotdot = set()
    scanned_files = 0

    def exists(path):
        return (root / path).is_file()

    for p, rel_str in core_sources(root):
        scanned_files += 1
        lines = p.read_text(encoding="utf-8", errors="ignore").splitlines()
        violations += scan_lines(rel_str, lines, exists, baseline)
        for line in lines:
            m = INCLUDE_PATTERN.match(line)
            if m and has_dotdot(m.group(2)):
                seen_dotdot.add((rel_str, m.group(2)))

    violations += stale_baseline(baseline, seen_dotdot)

    if verbose:
        print(f"Scanned {scanned_files} core source files.")

    return violations


def emit_dotdot_baseline(root_dir):
    root = Path(root_dir)
    pairs = set()
    for p, rel_str in core_sources(root):
        for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
            m = INCLUDE_PATTERN.match(line)
            if m and has_dotdot(m.group(2)):
                pairs.add((rel_str, m.group(2)))
    header = [
        "# Grandfathered '..' includes (tools/check_includes.py). One 'file<TAB>include' per line.",
        "# New '..' includes are refused; delete a line when its include is rewritten",
        "# root-relative (a stale line fails the check). Owner: M1 include hygiene.",
    ]
    DOTDOT_BASELINE.write_text(
        "\n".join(header + [f"{f}\t{i}" for f, i in sorted(pairs)]) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote {len(pairs)} entries to {DOTDOT_BASELINE}")


# (including file, include line, number of violations it must produce).
SELF_TEST_CASES = [
    ("core/packet/foo.h", '#include "framework/module.h"', 1),
    ("core/dataplane/bar.cc", '#include "runtime/runtime_state.h"', 1),
    ("core/classifier/exact.h", '#include "module.h"', 1),
    ("core/meter/meter.cc", '#include "control/api_v2.h"', 1),
    ("core/modules/foo.cc", '#include "runtime/runtime_state.h"', 1),
    ("core/dataplane/baz.h", '#include <google/protobuf/any.h>', 1),
    ("core/route/router.h", '#include "gate.h"', 1),
    ("core/dataplane/scope.h", '#include "meter/meter.h"', 1),
    ("core/dataplane/scope.h", '#include "route/next_hop_id.h"', 1),
    ("core/dataplane/slot_table.h", '#include "classifier/classifier.h"', 1),
    ("core/dataplane/batch_stages.h", '#include "stats/worker_slots.h"', 1),
    ("core/flow/worker_flow_table.h", '#include "module.h"', 1),
    ("core/flow/owner.h", '#include "stats/current_worker.h"', 1),
    ("core/dataplane/expiry_wheel.h", '#include "flow/flow_types.h"', 1),
    ("core/dataplane/expiry_wheel.h", '#include "worker.h"', 1),
    ("core/dataplane/tick_rate.h", '#include "gate.h"', 1),
    ("core/dataplane/handoff.h", '#include "packet_pool.h"', 1),
    ("core/dataplane/handoff.h", '#include "pktbatch.h"', 1),
    ("core/dataplane/continuation.h", '#include "packet.h"', 1),
    ("core/dataplane/handoff.cc", '#include "worker.h"', 1),
    ("core/dataplane/continuation.h", '#include "flow/flow_types.h"', 1),
    ("core/dataplane/handoff.h", '#include "framework/module_init_context.h"', 1),
    # A relative spelling is judged as the file it names: "../runtime/x.h" from
    # core/modules is core/runtime/x.h. Each also trips the '..' ban.
    ("core/modules/foo.cc", '#include "../runtime/runtime_state.h"', 2),
    ("core/dataplane/a.h", '#include "../../core/runtime/x.h"', 2),
    ("core/classifier/a.cc", '#include "../module.h"', 2),
    ("core/route/a.h", '#include "../control/service.h"', 2),
    # '..' is banned even where the target is allowed, and even in a test.
    ("core/dataplane/a.h", '#include "../utils/common.h"', 1),
    ("core/modules/new_module.cc", '#include "../utils/ip.h"', 1),
    ("core/flow/a_test.cc", '#include "../packet_pool.h"', 1),
    ("core/utils/a.h", '#include <../utils/b.h>', 1),
    # ... and only the resolved path is judged: this names utils/x.h.
    ("core/modules/a.cc", '#include "runtime/../utils/x.h"', 1),
]

# Includes that must pass: controls proving the rules are not over-broad.
SELF_TEST_CLEAN = [
    ("core/dataplane/a.h", '#include "utils/common.h"', frozenset()),
    ("core/dataplane/a.h", '#include "strong_id.h"', frozenset()),
    ("core/modules/a.cc", '#include "utils/ip.h"', frozenset()),
    ("core/modules/a.cc", '#include <glog/logging.h>', frozenset()),
    ("core/modules/a.cc", '#include "a..b/c.h"', frozenset()),
    ("core/modules/a.cc", '#include "./utils/ip.h"', frozenset()),
    # a layer's own directory name is not a forbidden edge
    ("core/flow/a.h", '#include "flow_key.h"', frozenset()),
    # tests are exempt from layering, not from the '..' ban
    ("core/dataplane/a_test.cc", '#include "runtime/runtime_state.h"', frozenset()),
    # a grandfathered pair passes the ban (and nothing else)
    ("core/modules/old.cc", '#include "../utils/ip.h"',
     frozenset({("core/modules/old.cc", "../utils/ip.h")})),
]


def nothing_exists(_path):
    return False


def run_self_test():
    """Negative self-test: the shared judge must flag every bad edge and only those."""
    print("Running check_includes self-test...")

    bad = 0
    for path, line, expected in SELF_TEST_CASES:
        got = scan_lines(path, [line], nothing_exists)
        if len(got) != expected:
            raise AssertionError(
                f"{path}: {line}: expected {expected} violation(s), got {len(got)}: {got}"
            )
        bad += expected

    for path, line, allowed in SELF_TEST_CLEAN:
        got = scan_lines(path, [line], nothing_exists, dotdot_allowed=allowed)
        if got:
            raise AssertionError(f"{path}: {line}: expected clean, got {got}")

    # The '..' baseline is exact: a listed pair passes, an unlisted pair (in the
    # same file, or the same include in another file) is still refused.
    pair = frozenset({("core/modules/old.cc", "../utils/ip.h")})
    line = '#include "../utils/ip.h"'
    for path, text, allowed, want in [
        ("core/modules/old.cc", line, frozenset(), 1),
        ("core/modules/old.cc", line, pair, 0),
        ("core/modules/old.cc", '#include "../utils/ether.h"', pair, 1),
        ("core/modules/new.cc", line, pair, 1),
    ]:
        got = scan_lines(path, [text], nothing_exists, dotdot_allowed=allowed)
        if len(got) != want:
            raise AssertionError(f"'..' baseline: {path} {text}: want {want}, got {got}")

    # Resolution is the compiler's: next to the includer first, then the root.
    # The rule below ("a module never includes another module's header") is
    # not shipped; it stands in for any rule whose fragment names a directory.
    siblings = [(re.compile(r"^core/modules/"), [("modules/", "no sibling modules")])]
    on_disk = {"core/modules/b.h", "core/utils/ip.h", "core/modules/utils/ip.h"}
    for path, text, tree, want in [
        # found next to the includer: it is modules/b.h
        ("core/modules/a.cc", '#include "b.h"', on_disk, 1),
        # not next to the includer, found under the root: utils/ip.h
        ("core/modules/a.cc", '#include "utils/ip.h"', on_disk - {"core/modules/utils/ip.h"}, 0),
        # found next to the includer first, even though the root has it too
        ("core/modules/a.cc", '#include "utils/ip.h"', on_disk, 1),
        # a <> include is never looked up next to the includer
        ("core/modules/a.cc", "#include <utils/ip.h>", on_disk, 0),
        ("core/modules/a.cc", "#include <modules/b.h>", on_disk, 1),
        # nothing found: judged as spelled under the root
        ("core/modules/a.cc", '#include "modules/c.h"', set(), 1),
        ("core/modules/a.cc", '#include "pb/x.pb.h"', set(), 0),
    ]:
        got = scan_lines(path, [text], lambda p, t=tree: p in t, rules=siblings)
        if len(got) != want:
            raise AssertionError(f"resolution: {path} {text}: want {want}, got {got}")

    # A baseline entry whose include is gone is an error: the list only shrinks.
    gone = ("core/modules/old.cc", "../utils/ip.h")
    if len(stale_baseline({gone, ("a.cc", "../b.h")}, {("a.cc", "../b.h")})) != 1 or (
        stale_baseline({gone}, {gone})
    ):
        raise AssertionError("stale baseline entries are not reported exactly")

    total = len(SELF_TEST_CASES)
    print(
        f"Self-test PASSED: {total} bad includes ({bad} violations) detected, "
        f"{len(SELF_TEST_CLEAN)} controls clean."
    )


def main():
    parser = argparse.ArgumentParser(
        description="BESS Layer Dependency Include Checker"
    )
    parser.add_argument(
        "--root",
        default=".",
        help="Root directory of BESS repository (default: '.')",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run negative self-test verifying detection of forbidden edges",
    )
    parser.add_argument(
        "--emit-dotdot-baseline",
        action="store_true",
        help="Rewrite include_dotdot_baseline.txt from the tree (shrink it, never grow it)",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Verbose output"
    )
    args = parser.parse_args()

    if args.emit_dotdot_baseline:
        emit_dotdot_baseline(args.root)
        return 0

    if args.self_test:
        run_self_test()
        return 0

    violations = check_includes(args.root, verbose=args.verbose)

    if violations:
        print(
            f"FAILED: Found {len(violations)} forbidden layer dependency edge(s):",
            file=sys.stderr,
        )
        for path, line_no, inc, reason in violations:
            print(
                f"  {path}:{line_no}: #include \"{inc}\"  -->  {reason}",
                file=sys.stderr,
            )
        return 1

    print("OK: 0 forbidden layer dependency edges found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
