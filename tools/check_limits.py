#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Every fixed limit is documented in docs/limits.md.

Scans production sources under core/ (not tests, benchmarks or fuzzers) for
limit-shaped constants -- `#define MAX_*/MIN_*/DEFAULT_*` and
`k{Max,Min,Default}*` constants -- and fails when docs/limits.md does not name
one. A new hardcoded limit therefore lands with its row in the central page.

Usage:
  tools/check_limits.py [--root DIR] [--self-test]
"""

import argparse
from pathlib import Path
import re
import sys

DEFINITION = re.compile(
    r'^\s*(?:#\s*define\s+((?:MAX|MIN|DEFAULT)_[A-Z0-9_]+)\b'
    r'|(?:(?:static|inline)\s+)*(?:constexpr|const)\s+[\w:<>]+\s+'
    r'(k(?:Max|Min|Default)[A-Za-z0-9]*)\s*=)')

SKIP = re.compile(r'_test\.cc$|_bench\.(?:cc|h)$|_fuzz|/fuzz/|/testing/')

# Limit-shaped names that are not limits.
NOT_LIMITS = {
    'kMax': 'local saturation bound in dataplane/tick_rate.h arithmetic',
    'kMinVer': 'pcapng format minor version (utils/pcapng.h)',
}


def definitions(text):
    for number, line in enumerate(text.splitlines(), 1):
        m = DEFINITION.match(line)
        if m:
            yield number, m.group(1) or m.group(2)


def undocumented(root):
    doc = (root / 'docs' / 'limits.md').read_text()
    missing = []
    for path in sorted((root / 'core').rglob('*')):
        if path.suffix not in ('.h', '.cc') or SKIP.search(path.as_posix()):
            continue
        for number, name in definitions(path.read_text(errors='replace')):
            if name in NOT_LIMITS:
                continue
            if not re.search(r'\b%s\b' % re.escape(name), doc):
                missing.append(f'{path.relative_to(root)}:{number}: {name}')
    return missing


def self_test():
    sample = '\n'.join([
        '#define MAX_GATES 8192',
        '  static constexpr size_t kMaxBatch = 64;',
        'static const int kDefaultQuantum =',
        'inline constexpr uint64_t kDefaultNatTimeout = 300;',
        'constexpr int kNotALimit = 3;',
        '#define BESS_ARCH_X86 1',
    ])
    names = [n for _, n in definitions(sample)]
    expected = ['MAX_GATES', 'kMaxBatch', 'kDefaultQuantum', 'kDefaultNatTimeout']
    if names != expected:
        print(f'self-test: got {names}, want {expected}')
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
    missing = undocumented(args.root)
    for entry in missing:
        print(f'{entry} is not in docs/limits.md')
    if missing:
        print(f'{len(missing)} limit(s) undocumented: add a row to docs/limits.md')
        return 1
    print('every limit is in docs/limits.md')
    return 0


if __name__ == '__main__':
    sys.exit(main())
