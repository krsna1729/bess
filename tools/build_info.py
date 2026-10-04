#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build metadata and an SPDX SBOM for one configured build (roadmap M26).

    build_info.py --build-dir BUILD --source-dir SRC --info OUT.json --sbom OUT.spdx.json

Both come from the build itself, never from hand-kept lists: meson's
introspection files (compilers, options, the dependencies found and their
versions), deps/dpdk.json (DPDK's pinned version, URL and sha256), the plugin
API version in core/framework/plugin.h, and git when the source is a checkout
("unknown" in a source release). bessd's install carries both files, so a
deployed daemon can say exactly what it is made of.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

OPTIONS = ('buildtype', 'cpp_std', 'cpu', 'arch_generic', 'b_sanitize', 'b_lto',
           'build_benchmarks', 'build_fuzzers')
# Found by meson but linked only into tests and benchmarks, never into bessd.
TEST_ONLY = frozenset({'gtest', 'gtest_main', 'gmock', 'gmock_main', 'benchmark',
                       'benchmark_main'})


def intro(build_dir, name):
    return json.loads((Path(build_dir) / 'meson-info' / f'intro-{name}.json').read_text())


def git(source_dir, *args):
    try:
        return subprocess.run(['git', '-C', str(source_dir), *args], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def plugin_api_version(source_dir):
    text = (Path(source_dir) / 'core' / 'framework' / 'plugin.h').read_text()
    match = re.search(r'#define BESS_PLUGIN_API_VERSION (\d+)', text)
    return int(match.group(1)) if match else None


def build_info(build_dir, source_dir):
    project = intro(build_dir, 'projectinfo')
    compilers = intro(build_dir, 'compilers').get('host', {})
    options = {o['name']: o['value'] for o in intro(build_dir, 'buildoptions')
               if o['name'] in OPTIONS}
    versions = {}
    for d in intro(build_dir, 'dependencies'):
        versions.setdefault(d['name'], set()).add(d.get('version') or 'unknown')
    dpdk = json.loads((Path(source_dir) / 'deps' / 'dpdk.json').read_text())
    commit = git(source_dir, 'rev-parse', 'HEAD')
    dirty = git(source_dir, 'status', '--porcelain', '--untracked-files=no')
    # A dirty build is not the commit's code: its tracked changes' digest
    # tells two dirty builds of one commit apart.
    diff = hashlib.sha256(git(source_dir, 'diff', 'HEAD').encode()).hexdigest() if dirty else None
    return {
        'name': project['descriptive_name'],
        'version': project['version'],
        'commit': commit or 'unknown',
        'dirty': bool(dirty) if commit else None,
        'dirty_diff_sha256': diff,
        'plugin_api_version': plugin_api_version(source_dir),
        'compilers': {lang: {'id': c['id'], 'version': c['version']}
                      for lang, c in sorted(compilers.items())},
        'options': options,
        'dpdk': {'version': dpdk['version'], 'url': dpdk['url'], 'sha256': dpdk['sha256']},
        'dependencies': [{'name': n, 'version': ' '.join(sorted(v))}
                         for n, v in sorted(versions.items())],
    }


def spdx_id(name):
    return 'SPDXRef-Package-' + re.sub(r'[^A-Za-z0-9.-]', '-', name)


def sbom(info):
    """An SPDX 2.3 document: BESS and what it was built against."""
    packages = [{
        'SPDXID': 'SPDXRef-Package-bess',
        'name': 'bess',
        'versionInfo': info['version'],
        'downloadLocation': 'NOASSERTION',
        'filesAnalyzed': False,
        'licenseDeclared': 'BSD-3-Clause',
        'comment': f"commit {info['commit']}",
    }, {
        'SPDXID': spdx_id('dpdk'),
        'name': 'dpdk',
        'versionInfo': info['dpdk']['version'],
        'downloadLocation': info['dpdk']['url'],
        'filesAnalyzed': False,
        'checksums': [{'algorithm': 'SHA256', 'checksumValue': info['dpdk']['sha256']}],
        'licenseDeclared': 'NOASSERTION',
    }]
    for dep in info['dependencies']:
        if dep['name'] == 'libdpdk':
            continue  # the pinned source above
        packages.append({
            'SPDXID': spdx_id(dep['name']),
            'name': dep['name'],
            'versionInfo': dep['version'],
            'downloadLocation': 'NOASSERTION',
            'filesAnalyzed': False,
            'licenseDeclared': 'NOASSERTION',
        })
    relationships = [{'spdxElementId': 'SPDXRef-Package-bess', 'relationshipType': 'DEPENDS_ON',
                      'relatedSpdxElement': p['SPDXID']}
                     if p['name'] not in TEST_ONLY else
                     {'spdxElementId': p['SPDXID'], 'relationshipType': 'TEST_DEPENDENCY_OF',
                      'relatedSpdxElement': 'SPDXRef-Package-bess'}
                     for p in packages[1:]]
    relationships.insert(0, {'spdxElementId': 'SPDXRef-DOCUMENT', 'relationshipType': 'DESCRIBES',
                             'relatedSpdxElement': 'SPDXRef-Package-bess'})
    digest = hashlib.sha256(json.dumps(info, sort_keys=True).encode()).hexdigest()
    return {
        'spdxVersion': 'SPDX-2.3',
        'dataLicense': 'CC0-1.0',
        'SPDXID': 'SPDXRef-DOCUMENT',
        'name': f"bess-{info['version']}",
        # Deterministic for the same inputs (reproducible builds): no random
        # UUID; the timestamp is SOURCE_DATE_EPOCH, else the commit's.
        'documentNamespace': f'https://github.com/krsna1729/bess/spdx/{digest}',
        'creationInfo': {'created': info.get('_created') or '1970-01-01T00:00:00Z',
                         'creators': ['Tool: bess-tools-build_info.py']},
        'packages': packages,
        'relationships': relationships,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--source-dir', required=True)
    parser.add_argument('--info', required=True)
    parser.add_argument('--sbom', required=True)
    args = parser.parse_args()
    info = build_info(args.build_dir, args.source_dir)
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    created = (datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc) if epoch
               else None)
    if created is None and (commit_time := git(args.source_dir, 'log', '-1', '--format=%cI',
                                               'HEAD')):
        created = datetime.datetime.fromisoformat(commit_time)
    doc_info = dict(info)
    if created:
        doc_info['_created'] = created.astimezone(datetime.timezone.utc).strftime(
            '%Y-%m-%dT%H:%M:%SZ')
    Path(args.info).write_text(json.dumps(info, indent=2, sort_keys=True) + '\n')
    Path(args.sbom).write_text(json.dumps(sbom(doc_info), indent=2, sort_keys=True) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
