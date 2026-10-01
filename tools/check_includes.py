#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS Layer Dependency Include Checker.

Enforces the BESS target architecture DAG (docs/architecture.md, Milestone M1)
by verifying that lower-level libraries do not include higher-level components
(e.g., pure libraries must not depend on Module, runtime state, or control RPCs).

Usage:
  tools/check_includes.py [--root DIR] [--self-test] [--verbose]
"""

import argparse
import os
from pathlib import Path
import re
import sys

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
    # Modules obtain runtime facilities through Module::init_context() (D-042),
    # never by including the runtime state. Tests and benchmarks are exempt.
    (
        re.compile(r"^core/modules/"),
        [
            ("runtime/", "modules must use Module::init_context(), not runtime"),
        ],
    ),
]


def check_includes(root_dir, verbose=False):
    root = Path(root_dir)
    violations = []
    scanned_files = 0

    include_pattern = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]')

    for p in root.glob("core/**/*.[ch]*"):
        rel_str = str(p.relative_to(root)).replace("\\", "/")

        # Unit tests and microbenchmarks are allowed integration fixtures
        if (
            "_test.cc" in rel_str
            or "_bench.cc" in rel_str
            or "gtest_main.cc" in rel_str
        ):
            continue

        scanned_files += 1
        with open(p, "r", encoding="utf-8", errors="ignore") as f:
            for line_no, line in enumerate(f, 1):
                m = include_pattern.match(line)
                if not m:
                    continue
                inc = m.group(1).replace("\\", "/")

                for file_re, rules in FORBIDDEN_RULES:
                    if file_re.search(rel_str):
                        for forbidden_sub, reason in rules:
                            if forbidden_sub in inc:
                                violations.append(
                                    (rel_str, line_no, inc, reason)
                                )
                                break

    if verbose:
        print(f"Scanned {scanned_files} core implementation files.")

    return violations


def run_self_test():
    """Negative self-test proving that forbidden edges are correctly caught."""
    print("Running check_includes self-test...")

    dummy_violations = []
    synthetic_cases = [
        ("core/packet/foo.h", '#include "framework/module.h"'),
        ("core/dataplane/bar.cc", '#include "runtime/runtime_state.h"'),
        ("core/classifier/exact.h", '#include "module.h"'),
        ("core/meter/meter.cc", '#include "control/api_v2.h"'),
        ("core/modules/foo.cc", '#include "runtime/runtime_state.h"'),
        ("core/dataplane/baz.h", '#include <google/protobuf/any.h>'),
        ("core/route/router.h", '#include "gate.h"'),
        ("core/dataplane/scope.h", '#include "meter/meter.h"'),
        ("core/dataplane/scope.h", '#include "route/next_hop_id.h"'),
        ("core/dataplane/slot_table.h", '#include "classifier/classifier.h"'),
        ("core/dataplane/batch_stages.h", '#include "stats/worker_slots.h"'),
    ]

    include_pattern = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]')

    for fake_path, line in synthetic_cases:
        m = include_pattern.match(line)
        inc = m.group(1)
        for file_re, rules in FORBIDDEN_RULES:
            if file_re.search(fake_path):
                for forbidden_sub, reason in rules:
                    if forbidden_sub in inc:
                        dummy_violations.append((fake_path, inc, reason))
                        break
    expected = 11
    assert len(synthetic_cases) == expected and len(dummy_violations) == expected, (
        f"Expected {expected} synthetic violations, got {len(dummy_violations)} "
        f"of {len(synthetic_cases)} cases"
    )
    print(f"Self-test PASSED: all {expected} synthetic violations correctly detected.")


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
        "--verbose", "-v", action="store_true", help="Verbose output"
    )
    args = parser.parse_args()

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
