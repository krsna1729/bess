#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS link-level layer checker (Milestone M1).

check_includes.py sees `#include` edges. This tool sees what actually links:
for every BESS static library it reads the archive symbol table, resolves each
undefined symbol against the strong definitions of the other BESS libraries,
and compares the resulting library-to-library graph with the allowlisted DAG in
tools/layer_dag.json. A dependency that enters through link configuration or a
non-header reference is caught here even if no forbidden include exists.

  tools/check_link_graph.py --build-dir build/gcc [--emit graph.json]
  tools/check_link_graph.py --self-test

Weak symbols (templates, inline functions) are ignored: they are duplicated in
every user and say nothing about a link dependency. Requires binutils `nm`.
"""

import argparse
import collections
import glob
import json
import os
from pathlib import Path
import re
import subprocess
import sys

DAG_FILE = Path(__file__).with_name("layer_dag.json")
STRONG_TYPES = set("TDBRSG")


def lib_name(path):
    m = re.match(r"^lib(bess_\w+)\.a$", os.path.basename(path))
    return m.group(1) if m else None


def archive_symbols(archive):
    """Returns (strong_definitions, undefined) symbol sets of one archive."""
    defined, undefined = set(), set()
    out = subprocess.run(
        ["nm", "-g", "--no-demangle", str(archive)],
        capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in STRONG_TYPES:
            defined.add(parts[2])
        elif len(parts) == 2 and parts[0] == "U":
            undefined.add(parts[1])
    return defined, undefined


def compute_edges(libs):
    """libs: {name: (defined, undefined)} -> {(from, to): shared_symbol_count}."""
    owner = {}
    for name, (defined, _) in libs.items():
        for symbol in defined:
            owner.setdefault(symbol, name)
    edges = collections.Counter()
    for name, (_, undefined) in libs.items():
        for symbol in undefined:
            other = owner.get(symbol)
            if other is not None and other != name:
                edges[(name, other)] += 1
    return edges


def check(edges, dag, libs_present):
    """Returns (violations, stale_exceptions, unknown_libs)."""
    allowed = {(a, b) for a, targets in dag["allowed"].items() for b in targets}
    excepted = {(e["from"], e["to"]) for e in dag["exceptions"]}
    for e in dag["exceptions"]:
        for key in ("owner", "remove_by", "reason"):
            if not e.get(key):
                raise ValueError(f"exception {e['from']}->{e['to']} lacks '{key}'")
    violations = sorted(
        (a, b, n) for (a, b), n in edges.items()
        if (a, b) not in allowed and (a, b) not in excepted)
    stale = sorted(e for e in excepted if e not in edges)
    unknown = sorted(n for n in libs_present if n not in dag["allowed"])
    return violations, stale, unknown


def find_cycles(edges, exceptions):
    """Cycles among the non-exception edges (the DAG proper)."""
    graph = collections.defaultdict(set)
    for (a, b) in edges:
        if (a, b) not in exceptions:
            graph[a].add(b)
    state, path, cycles = {}, [], []

    def visit(node):
        state[node] = 1
        path.append(node)
        for nxt in sorted(graph[node]):
            if state.get(nxt) == 1:
                cycles.append(path[path.index(nxt):] + [nxt])
            elif nxt not in state:
                visit(nxt)
        path.pop()
        state[node] = 2

    for node in sorted(graph):
        if node not in state:
            visit(node)
    return cycles


def load_libs(build_dir):
    libs = {}
    patterns = ["core/libbess_*.a", "protobuf/libbess_proto.a"]
    for pattern in patterns:
        for archive in sorted(glob.glob(str(Path(build_dir) / pattern))):
            name = lib_name(archive)
            if name:
                libs[name] = archive_symbols(archive)
    return libs


def run_self_test():
    print("Running check_link_graph self-test...")
    dag = {
        "allowed": {"a": [], "b": ["a"], "c": ["a", "b"]},
        "exceptions": [{"from": "a", "to": "c", "reason": "r", "owner": "o",
                        "remove_by": "x"}],
    }
    libs = {
        "a": ({"fa"}, {"fc"}),   # a -> c: excepted
        "b": ({"fb"}, {"fa"}),   # b -> a: allowed
        "c": ({"fc"}, {"fb"}),   # c -> b: allowed
    }
    edges = compute_edges(libs)
    violations, stale, unknown = check(edges, dag, libs)
    assert not violations and not stale and not unknown, (violations, stale)

    # An intentionally bad edge is a violation.
    libs["a"] = ({"fa"}, {"fb"})  # a -> b is in neither list
    violations, stale, _ = check(compute_edges(libs), dag, libs)
    assert violations == [("a", "b", 1)], violations
    assert stale == [("a", "c")], stale  # and the exception no longer occurs

    # Weak/undefined-only references do not create edges.
    assert not compute_edges({"a": (set(), {"zz"}), "b": ({"y"}, set())})

    # A cycle outside the exceptions is reported.
    cyc = find_cycles({("a", "b"): 1, ("b", "a"): 1}, set())
    assert cyc and cyc[0][0] == cyc[0][-1], cyc
    assert not find_cycles({("a", "b"): 1, ("b", "a"): 1}, {("b", "a")})

    # An exception without an owner is refused.
    try:
        check({}, {"allowed": {}, "exceptions": [{"from": "a", "to": "b"}]}, {})
    except ValueError:
        pass
    else:
        raise AssertionError("ownerless exception accepted")
    print("Self-test PASSED: bad edge, stale exception, weak symbols, cycles "
          "and ownerless exceptions handled.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build-dir", help="configured and built Meson directory")
    parser.add_argument("--dag", default=str(DAG_FILE))
    parser.add_argument("--emit", help="write the computed dependency graph (JSON)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return 0
    if not args.build_dir:
        parser.error("--build-dir is required")

    dag = json.loads(Path(args.dag).read_text())
    libs = load_libs(args.build_dir)
    if not libs:
        print(f"ERROR: no libbess_*.a archives under {args.build_dir}")
        return 2
    edges = compute_edges(libs)
    violations, stale, unknown = check(edges, dag, libs)
    exceptions = {(e["from"], e["to"]) for e in dag["exceptions"]}
    cycles = find_cycles(edges, exceptions)

    if args.emit:
        graph = {
            "nodes": sorted(libs),
            "edges": [
                {"from": a, "to": b,
                 "status": "exception" if (a, b) in exceptions else "allowed"}
                for (a, b) in sorted(edges)
            ],
            "exceptions": dag["exceptions"],
        }
        Path(args.emit).write_text(json.dumps(graph, indent=2) + "\n")

    for name in unknown:
        print(f"ERROR: library {name} is not in the allowlisted DAG ({DAG_FILE.name})")
    for a, b, n in violations:
        owned = libs[b][0]
        symbols = sorted(s for s in libs[a][1] if s in owned)
        shown = subprocess.run(["c++filt"], input="\n".join(symbols[:5]), capture_output=True,
                               text=True).stdout.split("\n") if symbols else []
        print(f"ERROR: forbidden link edge {a} -> {b} ({n} symbols) "
              f"is in neither 'allowed' nor 'exceptions'; first: "
              + "; ".join(s for s in shown if s))
    for cycle in cycles:
        print("ERROR: dependency cycle outside the exceptions: " + " -> ".join(cycle))
    for a, b in stale:
        print(f"WARNING: exception {a} -> {b} no longer occurs; remove it from "
              f"{DAG_FILE.name}")
    if violations or unknown or cycles:
        return 1
    print(f"OK: {len(libs)} libraries, {len(edges)} link edges "
          f"({len(exceptions & set(edges))} grandfathered), no violations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
