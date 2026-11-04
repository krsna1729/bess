#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Every meson test declares memory; scheduled declarations fit 2048 MiB.

Reads a build's test list (meson-info/intro-tests.json) and the declarations
(tests/memory/meson.build, test_memory_mb) and fails when:

  * a test has no BESS_TEST_MEM_MB in its environment, or zero (no entry);
  * an entry is above CEILING_MB without a `# above the ceiling:` comment just
    above it saying what it proves that a smaller one could not;
  * the scheduled declaration sum for `meson test -jN`, N in GATED_JOBS,
    exceeds SUITE_LIMIT_MB: the largest of (the largest is_parallel: false
    test) and (the N largest parallel tests, run together);
  * docs/testing.md's table does not list a test with its declaration.

The example projects (examples/*/meson.build) are built from the installed
tree, not in this build: each of their test() calls must set BESS_TEST_MEM_MB.

Declarations bound run_capped.py's cache-adjusted diagnostic, not gross cgroup
memory.peak. This scheduling check is not a whole-suite resident-memory cap;
only a capped parent slice enforces that (docs/testing.md#the-suite).

Usage:
  tools/check_test_memory.py --build-dir BUILD [--root DIR]
  tools/check_test_memory.py --peaks BUILD [--json]
  tools/check_test_memory.py --self-test

--peaks prints what the last `meson test` in BUILD measured: each test's
declaration and the peak tools/run_capped.py logged for it, largest first
(--json: {name: {"declared": MiB, "peak": MiB}}).
"""

import argparse
import json
from pathlib import Path
import re
import sys

CEILING_MB = 1024
SUITE_LIMIT_MB = 2048
GATED_JOBS = (1, 3)
ENV = 'BESS_TEST_MEM_MB'
JUSTIFIED = '# above the ceiling:'
ENTRY = re.compile(r"^\s*'([^']+)'\s*:\s*(\d+)\s*,")
DOC_ROW = re.compile(r'^\|\s*`([^`]+)`\s*\|\s*(\d+)\s*\|')


def declarations(table_text):
    """{name: (MiB, justified)} from test_memory_mb's entries."""
    out = {}
    comment = []
    for line in table_text.splitlines():
        stripped = line.strip()
        if stripped.startswith('#'):
            comment.append(stripped)
            continue
        m = ENTRY.match(line)
        if m:
            justified = any(c.startswith(JUSTIFIED) and len(c) > len(JUSTIFIED) + 10
                            for c in comment)
            out[m.group(1)] = (int(m.group(2)), justified)
        comment = []
    return out


def bound(tests, jobs):
    """Scheduled declaration sum at `meson test -j jobs`, MiB, and which tests."""
    serial = sorted(((t['mb'], t['name']) for t in tests if not t['parallel']), reverse=True)
    parallel = sorted(((t['mb'], t['name']) for t in tests if t['parallel']), reverse=True)
    alone = serial[:1]
    together = parallel[:jobs] if jobs > 1 else parallel[:1]
    a = sum(mb for mb, _ in alone)
    b = sum(mb for mb, _ in together)
    return (a, [n for _, n in alone]) if a >= b else (b, [n for _, n in together])


def documented(doc_text):
    return {m.group(1): int(m.group(2)) for m in map(DOC_ROW.match, doc_text.splitlines()) if m}


def test_calls(text):
    """Each test(...) call's text, parentheses balanced."""
    for m in re.finditer(r'(?m)^\s*test\(', text):
        depth, i = 0, m.end() - 1
        for i in range(m.end() - 1, len(text)):
            depth += {'(': 1, ')': -1}.get(text[i], 0)
            if depth == 0:
                break
        yield text[m.end():i]


def check(intro, table_text, doc_text, examples, jobs=GATED_JOBS):
    problems = []
    table = declarations(table_text)
    example_tests = []  # their own projects: each runs alone, outside the -jN bound
    for path, text in examples:
        for call in test_calls(text):
            name = call.split(',', 1)[0].strip().strip("'")
            m = re.search(r"'%s'\s*:\s*'(\d+)'" % ENV, call)
            if not m or int(m.group(1)) <= 0:
                problems.append(f"{path}: test('{name}') sets no {ENV}")
                continue
            table[name] = (int(m.group(1)), False)
            example_tests.append({'name': name, 'mb': int(m.group(1)), 'parallel': False})
    for name, (mb, justified) in sorted(table.items()):
        if mb > SUITE_LIMIT_MB:
            problems.append(f'{name}: {mb} MiB is above the suite limit {SUITE_LIMIT_MB} MiB '
                            'on its own; shrink it')
        elif mb > CEILING_MB and not justified:
            problems.append(f'{name}: {mb} MiB is above the {CEILING_MB} MiB ceiling with no '
                            f'"{JUSTIFIED} <what a smaller N could not prove>" comment')
    tests = []
    for t in intro:
        value = t.get('env', {}).get(ENV, '')
        if not value.isdigit() or int(value) <= 0:
            problems.append(f'{t["name"]}: no memory declaration ({ENV}={value!r}): add it to '
                            'test_memory_mb in tests/memory/meson.build')
            continue
        tests.append({'name': t['name'], 'mb': int(value), 'parallel': t['is_parallel']})
    for n in jobs:
        mb, names = bound(tests, n)
        if mb > SUITE_LIMIT_MB:
            problems.append(f'meson test -j{n} can hold {mb} MiB at once ({", ".join(names)}), '
                            f'above {SUITE_LIMIT_MB} MiB: shrink them or make them '
                            'is_parallel: false')
    docs = documented(doc_text)
    for t in tests + example_tests:
        if docs.get(t['name']) != t['mb']:
            problems.append(f'docs/testing.md: no row "| `{t["name"]}` | {t["mb"]} |"'
                            + (f' (it says {docs[t["name"]]})' if t['name'] in docs else ''))
    for name in sorted(set(docs) - set(table)):
        problems.append(f'docs/testing.md: `{name}` has a row but no declaration')
    return problems, tests


def run(root, build_dir):
    intro = json.loads((build_dir / 'meson-info' / 'intro-tests.json').read_text())
    table = (root / 'tests' / 'memory' / 'meson.build').read_text()
    doc = (root / 'docs' / 'testing.md').read_text()
    examples = [(p.relative_to(root), p.read_text())
                for p in sorted((root / 'examples').glob('*/meson.build'))]
    problems, tests = check(intro, table, doc, examples)
    for n in sorted({*GATED_JOBS, 4}):
        mb, names = bound(tests, n)
        print(f'meson test -j{n}: scheduled declarations {mb} MiB ({", ".join(names)})')
    for p in problems:
        print(p)
    return 1 if problems else 0


PEAK = re.compile(r'run_capped: [^:]*: declared (\d+) MiB, peak ([0-9.]+) MiB')


def peaks(build_dir, as_json):
    """What meson's last run logged (meson-logs/testlog.json)."""
    out = {}
    log = build_dir / 'meson-logs' / 'testlog.json'
    for line in log.read_text().splitlines():
        entry = json.loads(line)
        text = (entry.get('stdout') or '') + (entry.get('stderr') or '')
        found = PEAK.findall(text)
        if found:
            declared, peak = found[-1]
            # Meson versions use "SUITE - PROJECT:NAME" or "PROJECT / NAME".
            name = entry['name'].rpartition(':')[2].rpartition(' / ')[2]
            out[name] = {'declared': int(declared), 'peak': float(peak)}
    if as_json:
        print(json.dumps(out, indent=1, sort_keys=True))
        return 0
    print(f'{"test":<56} {"declared":>9} {"peak":>9}  MiB')
    for name, v in sorted(out.items(), key=lambda kv: -kv[1]['peak']):
        print(f'{name:<56} {v["declared"]:>9} {v["peak"]:>9.1f}')
    return 0


def self_test():
    table = """test_memory_mb = {
  'small': 64,
  'par_a': 600,
  'par_b': 600,
  # above the ceiling: it proves the growth at the scale the bug needed
  'big': 1500,
  'bare_big': 1100,
}"""
    intro = [
        {'name': 'small', 'env': {ENV: '64'}, 'is_parallel': False},
        {'name': 'big', 'env': {ENV: '1500'}, 'is_parallel': False},
        {'name': 'par_a', 'env': {ENV: '600'}, 'is_parallel': True},
        {'name': 'par_b', 'env': {ENV: '600'}, 'is_parallel': True},
        {'name': 'missing', 'env': {ENV: '0'}, 'is_parallel': False},
    ]
    doc = ('| `small` | 64 |\n| `big` | 1500 |\n| `par_a` | 600 |\n| `par_b` | 9 |\n'
           '| `gone` | 1 |\n| `ok` | 32 |\n')
    examples = [('examples/x/meson.build',
                 "test('ok', exe, env: {'BESS_TEST_MEM_MB': '32'})\ntest('bare', exe)\n")]
    problems, tests = check(intro, table, doc, examples)
    want = {
        'bare_big: 1100 MiB is above the 1024 MiB ceiling': True,
        'big: 1500': False,  # justified
        'missing: no memory declaration': True,
        'docs/testing.md: no row "| `par_b` | 600 |" (it says 9)': True,
        'docs/testing.md: `gone` has a row but no declaration': True,
        "examples/x/meson.build: test('bare') sets no": True,
        "examples/x/meson.build: test('ok')": False,
        'docs/testing.md: no row "| `ok`': False,
        'docs/testing.md: `ok`': False,
        'meson test -j': False,  # 1500 alone, 1200 together: within 2048
    }
    failures = [f'{"missing" if expected else "unexpected"}: {needle}'
                for needle, expected in want.items()
                if any(p.startswith(needle) for p in problems) != expected]
    if bound(tests, 1) != (1500, ['big']) or bound(tests, 3) != (1500, ['big']):
        failures.append(f'bound: {bound(tests, 1)} {bound(tests, 3)}')
    over = intro + [{'name': 'par_c', 'env': {ENV: '900'}, 'is_parallel': True}]
    problems, _ = check(over, table + "\n  'par_c': 900,", doc, [])
    if not any(p.startswith('meson test -j3 can hold 2100 MiB') for p in problems):
        failures.append(f'-j3 over the limit not reported: {problems}')
    if any(p.startswith('meson test -j1') for p in problems):
        failures.append('-j1 reported over the limit')
    for f in failures:
        print('FAIL', f)
    print('self-test: ' + ('FAILED' if failures else 'ok'))
    return 1 if failures else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--peaks', type=Path, metavar='BUILD')
    p.add_argument('--json', action='store_true')
    p.add_argument('--self-test', action='store_true')
    args = p.parse_args()
    if args.self_test:
        return self_test()
    if args.peaks:
        return peaks(args.peaks, args.json)
    if not args.build_dir:
        p.error('--build-dir is required')
    return run(args.root, args.build_dir)


if __name__ == '__main__':
    sys.exit(main())
