#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Growth that scales with entries names its bound.

Scans production sources under core/ (not tests, benchmarks or fuzzers) for
the shapes behind the DSA audit's scale bugs and fails unless the line, or
the comment block directly above it, says why the cost is bounded with
`// scale: <bound or reason>`:

  exact-growth       `.reserve(... + ...)` or `.resize(x.size() + ...)`: an
                     exact-size grow per call reallocates every time, so N
                     calls cost N^2/2 moves (RecordArena, CL-1).
  front-erase        `.erase(x.begin()...)`, or `X.erase(` inside a loop where
                     X is a std::vector of the file or its header: every
                     erase moves the tail (FM-7, CL-8).
  reserve-path-copy  in a Reserve/Prepare*/Stage*/Upsert*/Erase* function, a
                     `make_unique/make_shared<T>(*x)` copy or a container
                     copy-initialized from a member: a transaction pays for
                     what is installed, not for what it changes (DP-2).

Usage:
  tools/check_scaling.py [--root DIR] [--self-test]
"""

import argparse
from pathlib import Path
import re
import sys

SKIP = re.compile(r'_test\.cc$|_bench\.(?:cc|h)$|_fuzz|/fuzz/|/testing/')

EXEMPT = re.compile(r'//\s*scale:\s*\S')

EXACT_GROWTH = re.compile(
    r'\.reserve\([^;]*\+|\.resize\([^;]*\.size\(\)\s*\+')
FRONT_ERASE = re.compile(r'\.erase\(\s*[\w.>\-]+\.begin\(\)')
LOOP_ERASE = re.compile(r'(?:^|[^\w.>])([A-Za-z_]\w*)(?:\.|->)erase\(')
VECTOR_DECL = re.compile(
    r'^\s*(?:static\s+|mutable\s+|inline\s+)*std::vector<[^;]*>\s+([A-Za-z_]\w*)\s*[;={(]',
    re.M)
LOOP_KEYWORD = re.compile(r'\b(?:for|while)\s*\(')

STAGING = r'(?:Reserve|Prepare\w*|Stage\w*|BeginStage|Upsert\w*|Erase\w*)'
# A definition line: an optional return type, an optional Class:: qualifier,
# then the name and its parameter list; never a call through ., -> or return.
STAGING_DEF = re.compile(
    r'^\s*(?!return\b)(?:[\w:<>,*&]+\s+)*(?:\w+::)*' + STAGING + r'\s*\(')
POINTER_COPY = re.compile(
    r'make_(?:unique|shared)<[^;]*?>\(\s*\*(?!this\b)[\w.>\-]+\s*\)')
CONTAINER_COPY = re.compile(
    r'(?:std::)?(?:map|set|unordered_\w+|vector)<.*>\s+\w+\s*[=(]\s*\*?\w+_?\s*[;)]')


def strip_comments(text):
    """Blanks comments and string literals, keeping line numbers and columns."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + ' ' * (j - i - 2) + (c if j - i >= 2 else ''))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def scopes(code):
    """Per line: (in a loop body, name of the enclosing staging function)."""
    lines = code.split('\n')
    in_loop = [False] * len(lines)
    staging = [None] * len(lines)
    stack = []  # entries: 'loop', 'block', or ('staging', name)
    pending_loop = False
    pending_staging = None
    paren = 0
    for number, line in enumerate(lines):
        m = STAGING_DEF.match(line)
        if m and not stack_has_staging(stack):
            pending_staging = re.search(STAGING + r'\s*\(', line).group(0)[:-1].strip()
        line_loop = any(e == 'loop' for e in stack)
        for k, ch in enumerate(line):
            if LOOP_KEYWORD.match(line, k) and (k == 0 or not (line[k - 1].isalnum() or line[k - 1] == '_')):
                pending_loop = True
            if ch == '(':
                paren += 1
            elif ch == ')':
                paren = max(0, paren - 1)
            elif ch == '{':
                if pending_staging is not None:
                    stack.append(('staging', pending_staging))
                    pending_staging = None
                elif pending_loop and paren == 0:
                    stack.append('loop')
                    pending_loop = False
                else:
                    stack.append('block')
            elif ch == '}':
                if stack:
                    stack.pop()
            elif ch == ';' and paren == 0:
                if pending_loop:
                    line_loop = True  # single-statement body
                pending_loop = False
                pending_staging = None
        in_loop[number] = line_loop or any(e == 'loop' for e in stack)
        staging[number] = next((e[1] for e in stack if isinstance(e, tuple)), None)
    return in_loop, staging


def stack_has_staging(stack):
    return any(isinstance(e, tuple) for e in stack)


def vectors(text):
    return {m.group(1) for m in VECTOR_DECL.finditer(strip_comments(text))}


def exempt(raw, i):
    """The line, or the comment block directly above it, names the bound."""
    if EXEMPT.search(raw[i]):
        return True
    j = i - 1
    while j >= 0 and raw[j].lstrip().startswith('//'):
        if EXEMPT.search(raw[j]):
            return True
        j -= 1
    return False


def exits_after(lines, i):
    """The statement after line i leaves the loop: one erase per call."""
    for line in lines[i + 1:]:
        if line.strip():
            return bool(re.match(r'\s*(?:return\b|break\s*;)', line))
    return False


def findings(text, header_text=''):
    raw = text.split('\n')
    code = strip_comments(text)
    lines = code.split('\n')
    in_loop, staging = scopes(code)
    vecs = vectors(text) | vectors(header_text)
    for i, line in enumerate(lines):
        rules = []
        if EXACT_GROWTH.search(line):
            rules.append('exact-growth')
        if FRONT_ERASE.search(line):
            rules.append('front-erase')
        elif (in_loop[i] and not exits_after(lines, i) and
              any(m.group(1) in vecs for m in LOOP_ERASE.finditer(line))):
            rules.append('front-erase')
        if staging[i] and (POINTER_COPY.search(line) or CONTAINER_COPY.search(line)):
            rules.append('reserve-path-copy')
        if not rules:
            continue
        if exempt(raw, i):
            continue
        for rule in rules:
            yield i + 1, rule, raw[i].strip()


def scan(root):
    found = []
    for path in sorted((root / 'core').rglob('*')):
        if path.suffix not in ('.h', '.cc') or SKIP.search(path.as_posix()):
            continue
        header = path.with_suffix('.h')
        header_text = header.read_text(errors='replace') if path.suffix == '.cc' and header.exists() else ''
        for number, rule, line in findings(path.read_text(errors='replace'), header_text):
            found.append(f'{path.relative_to(root)}:{number}: {rule}: {line}')
    return found


def self_test():
    cases = [
        # (source, header, expected rules)
        ('v.reserve(v.size() + 1);', '', ['exact-growth']),
        ('v.reserve(v.size() + 1);  // scale: <= kMaxPending (4096), once', '', []),
        ('// scale: <= kMaxPending (4096), once\nv.reserve(v.size() + 1);', '', []),
        ('// scale: half the queue is dead, so a compaction\n// moves no more than it drops.\nv.reserve(v.size() + 1);', '', []),
        ('// scale: bounded\nx = 1;\nv.reserve(v.size() + 1);', '', ['exact-growth']),
        ('v.reserve(v.size() + 1);  // scale:', '', ['exact-growth']),
        ('v.resize(v.size() + n);', '', ['exact-growth']),
        ('v.reserve(n);', '', []),
        ('q.erase(q.begin());', '', ['front-erase']),
        ('q_.erase(q_.begin(), q_.begin() + k);', '', ['front-erase']),
        ('void F() {\n  for (auto &x : xs) {\n    items_.erase(it);\n  }\n}',
         'std::vector<int> items_;', ['front-erase']),
        ('void F() {\n  for (auto &x : xs) {\n    index_.erase(x);\n  }\n}',
         'std::map<int, int> index_;', []),
        ('void F() {\n  for (auto &x : xs) {\n    index_.erase(x);\n  }\n}',
         'std::unordered_map<int, std::vector<int>> index_;', []),
        ('void F() {\n  while (more())\n    items_.erase(it);\n}',
         'std::vector<int> items_;', ['front-erase']),
        ('void F() {\n  items_.erase(it);\n}', 'std::vector<int> items_;', []),
        ('void F() {\n  for (auto it = items_.begin(); it != items_.end(); ++it) {\n'
         '    if (*it == x) {\n      items_.erase(it);\n      return;\n    }\n  }\n}',
         'std::vector<int> items_;', []),
        ('Status T::Reserve(const Op &op) {\n  auto x = std::make_unique<Op>(*this);\n}',
         '', []),
        ('Status T::Reserve(const Op &op) {\n  auto m = std::make_unique<Map>(*published_);\n}',
         '', ['reserve-path-copy']),
        ('Status T::PrepareUpsert(const Op &op) {\n  std::map<int, int> copy = flows_;\n}',
         '', ['reserve-path-copy']),
        ('Status T::Lookup(const Op &op) {\n  auto m = std::make_unique<Map>(*published_);\n}',
         '', []),
        ('void F() {\n  table->Reserve(op, [&] {\n    auto m = std::make_unique<Map>(*p_);\n  });\n}',
         '', []),
        ('const char *s = "v.reserve(v.size() + 1)";', '', []),
        ('/* v.reserve(v.size() + 1); */', '', []),
    ]
    failed = 0
    for source, header, want in cases:
        got = [rule for _, rule, _ in findings(source, header)]
        if got != want:
            print(f'self-test: {source!r}: got {got}, want {want}')
            failed += 1
    if failed:
        return 1
    print('self-test: ok')
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    p.add_argument('--self-test', action='store_true')
    args = p.parse_args()
    if args.self_test:
        return self_test()
    found = scan(args.root)
    for entry in found:
        print(entry)
    if found:
        print(f'{len(found)} unbounded-growth shape(s): fix, or name the bound '
              'with `// scale: <bound or reason>` on the line or the line above')
        return 1
    print('every scaling shape names its bound')
    return 0


if __name__ == '__main__':
    sys.exit(main())
