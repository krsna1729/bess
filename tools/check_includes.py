#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS Layer Dependency Include Checker.

Enforces the BESS target architecture DAG (docs/architecture.md, Milestone M1)
by verifying that lower-level libraries do not include higher-level components
(e.g., pure libraries must not depend on Module, runtime state, or control RPCs).

Usage:
  tools/check_includes.py [--root DIR] [--self-test] [--verbose]

Each quoted include is resolved the way the compiler does (relative to the
including file, then the `core/` include root) and the rules are applied to the
resolved path. Every `..` component is banned in quoted and angle includes.
"""

import argparse
from pathlib import Path
import posixpath
import re
import sys

INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s+(["<])([^">]+)[">]')

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
    # The L2 library (M14, D-064) is graph-independent: no Module, gate,
    # framework, runtime, control or protobuf; interfaces are InterfaceId.
    (
        re.compile(r"^core/l2/"),
        [
            ("module.h", "L2 library must not depend on Module"),
            ("gate.h", "L2 library must not depend on gates; use InterfaceId"),
            ("framework/", "L2 library must not depend on framework"),
            ("runtime/", "L2 library must not depend on runtime"),
            ("control/", "L2 library must not depend on control"),
            ("pb/", "L2 library must not depend on protobuf"),
        ],
    ),
    # The conntrack library (M17, D-067) is a networking library over flow and
    # expiry: no Module, gate, framework, runtime, control or protobuf.
    (
        re.compile(r"^core/conntrack/"),
        [
            ("module.h", "conntrack must not depend on Module"),
            ("gate.h", "conntrack must not depend on gates"),
            ("framework/", "conntrack must not depend on framework"),
            ("runtime/", "conntrack must not depend on runtime"),
            ("control/", "conntrack must not depend on control"),
            ("pb/", "conntrack must not depend on protobuf"),
        ],
    ),
    # The NAT library (M18, D-068): no Module, gate, framework, runtime, control
    # or protobuf.
    (
        re.compile(r"^core/nat/"),
        [
            ("module.h", "NAT library must not depend on Module"),
            ("gate.h", "NAT library must not depend on gates"),
            ("framework/", "NAT library must not depend on framework"),
            ("runtime/", "NAT library must not depend on runtime"),
            ("control/", "NAT library must not depend on control"),
            ("pb/", "NAT library must not depend on protobuf"),
        ],
    ),
    # The tunnel library (M19, D-069): no Module, gate, framework, runtime,
    # control, protobuf or metadata.
    (
        re.compile(r"^core/tunnel/"),
        [
            ("module.h", "tunnel library must not depend on Module"),
            ("gate.h", "tunnel library must not depend on gates"),
            ("metadata.h", "tunnel library must not depend on metadata"),
            ("framework/", "tunnel library must not depend on framework"),
            ("runtime/", "tunnel library must not depend on runtime"),
            ("control/", "tunnel library must not depend on control"),
            ("pb/", "tunnel library must not depend on protobuf"),
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
    # The EAL layer (bess_eal, D-057): DPDK bring-up, memory, process options,
    # thread placement, and the two utils files that start the EAL lazily. It is
    # below the worker, the packet pool and the registries, so none of them may
    # be included. Tests and benchmarks are exempt.
    (
        re.compile(
            r"^core/runtime/(dpdk|memory|opts|path|startup|thread_placement)\.(cc|h)$"
            r"|^core/utils/(dpdk_memory|bpf_program)\.(cc|h)$"
        ),
        [
            ("worker.h", "the EAL layer must not depend on the worker"),
            ("module.h", "the EAL layer must not depend on Module"),
            ("packet_pool.h", "the EAL layer must not depend on the packet pool"),
            ("scheduler.h", "the EAL layer must not depend on the scheduler"),
            ("traffic_class.h", "the EAL layer must not depend on traffic classes"),
            ("runtime_state.h", "the EAL layer must not depend on the runtime registries"),
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


# The one file allowed to include glog directly (see its comment).
LOGGING_HEADER = "core/utils/logging.h"


def judge_include(rel_path, quote, inc, exists, layering=True, rules=None):
    """Return (reason, ...) for every rule the include breaks."""
    reasons = []
    if has_dotdot(inc):
        reasons.append(
            "'..' in an include path is banned: spell it relative to the "
            "include root (core/)"
        )
    if inc == "glog/logging.h" and rel_path != LOGGING_HEADER:
        reasons.append(
            "include glog through \"utils/logging.h\", which puts it after "
            "absl logging so LOG/CHECK use glog in every translation unit"
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


def scan_lines(rel_path, lines, exists, rules=None):
    """Violations (path, line, include, reason) of one file's lines."""
    violations = []
    layering = not is_fixture(rel_path)
    for line_no, line in enumerate(lines, 1):
        m = INCLUDE_PATTERN.match(line)
        if not m:
            continue
        inc = m.group(2).replace("\\", "/")
        for reason in judge_include(rel_path, m.group(1), inc, exists, layering, rules):
            violations.append((rel_path, line_no, inc, reason))
    return violations


def core_sources(root):
    for p in sorted(root.glob("core/**/*.[ch]*")):
        yield p, str(p.relative_to(root)).replace("\\", "/")


def check_includes(root_dir, verbose=False):
    root = Path(root_dir)
    violations = []
    scanned_files = 0

    def exists(path):
        return (root / path).is_file()

    for p, rel_str in core_sources(root):
        scanned_files += 1
        lines = p.read_text(encoding="utf-8", errors="ignore").splitlines()
        violations += scan_lines(rel_str, lines, exists)

    if verbose:
        print(f"Scanned {scanned_files} core source files.")

    return violations


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
    ("core/l2/fdb.h", '#include "gate.h"', 1),
    ("core/conntrack/conntrack.h", '#include "gate.h"', 1),
    ("core/nat/nat.h", '#include "module.h"', 1),
    ("core/tunnel/tunnel.h", '#include "metadata.h"', 1),
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
    # `..` is banned even where the target is allowed, in tests, and after
    # normalisation; quoted and angle forms are both covered.
    ("core/dataplane/a.h", '#include "../utils/common.h"', 1),
    ("core/modules/new_module.cc", '#include "../utils/ip.h"', 1),
    ("core/flow/a_test.cc", '#include "../packet_pool.h"', 1),
    ("core/utils/a.h", '#include <../utils/b.h>', 1),
    ("core/utils/a.h", '#include <a/../utils/b.h>', 1),
    ("core/modules/a.cc", '#include "runtime/../utils/x.h"', 1),
    ("core/modules/a.cc", '#include <a/../utils/x.h>', 1),
    # The EAL layer (bess_eal, D-057) sits below the worker and the registries.
    ("core/runtime/opts.cc", '#include "worker.h"', 1),
    ("core/runtime/dpdk.cc", '#include "packet_pool.h"', 1),
    ("core/utils/dpdk_memory.cc", '#include "runtime/runtime_state.h"', 1),
    ("core/runtime/thread_placement.cc", '#include "scheduler.h"', 1),
    # glog only through utils/logging.h, which orders it after absl logging;
    # tests included (they log too).
    ("core/modules/a.cc", '#include <glog/logging.h>', 1),
    ("core/dataplane/a_test.cc", '#include <glog/logging.h>', 1),
    ("core/utils/other.h", '#include "glog/logging.h"', 1),
]

# Includes that must pass: controls proving the rules are not over-broad.
SELF_TEST_CLEAN = [
    ("core/dataplane/a.h", '#include "utils/common.h"'),
    ("core/dataplane/a.h", '#include "strong_id.h"'),
    ("core/modules/a.cc", '#include "utils/ip.h"'),
    ("core/modules/a.cc", '#include <rte_mbuf.h>'),
    ("core/modules/a.cc", '#include "utils/logging.h"'),
    ("core/utils/logging.h", '#include <glog/logging.h>'),
    ("core/modules/a.cc", '#include "a..b/c.h"'),
    ("core/modules/a.cc", '#include "./utils/ip.h"'),
    # a layer's own directory name is not a forbidden edge
    ("core/flow/a.h", '#include "flow_key.h"'),
    # tests are exempt from layering, not from the '..' ban
    ("core/dataplane/a_test.cc", '#include "runtime/runtime_state.h"'),
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

    for path, line in SELF_TEST_CLEAN:
        got = scan_lines(path, [line], nothing_exists)
        if got:
            raise AssertionError(f"{path}: {line}: expected clean, got {got}")

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
