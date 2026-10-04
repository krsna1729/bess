#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Load the installed-tree conformance plugins into the staged bessd and run
packets through each (roadmap M23).

verify-install compiles examples/standalone_plugin from the staged bess-dev
alone; this proves the result also loads into that install's bessd (every
symbol the plugin takes from bessd resolves), registers its module class, and
moves packets: Source -> <plugin module> -> Sink on one worker, for a short
run, and the plugin's output edge must have carried packets.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

# module class -> how many instances (each its own Source -> module -> Sink;
# every Init takes EmptyArg today).
PLUGIN_CLASSES = {
    'StandalonePass': 1,
    'StandaloneMacSwap': 1,
    'StandaloneRangeGate': 1,
    'StandaloneFlowCount': 1,  # gate 0 carries packets of flows it already knows
    'StandaloneAppliance': 2,  # the second instance looks up the first's policy graph
}


def wait_connected(client, process, url, log_path):
    deadline = time.monotonic() + 30
    last_error = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'bessd exited with {process.returncode}:\n'
                               + log_path.read_text(errors='replace')[-3000:])
        try:
            client.connect(url)
            client.list_mclasses()
            return
        except Exception as error:  # gRPC reports several transient errors
            last_error = error
            client.disconnect()
            time.sleep(0.1)
    raise RuntimeError(f'timed out connecting to bessd: {last_error}')


def edge_packets(client, module, gate=0):
    """Packets that left `module` on output `gate`."""
    info = client.get_module_info(module)
    for ogate in info.ogates:
        if ogate.ogate == gate:
            return ogate.pkts
    return 0


def free_port():
    """A port nothing listens on now, so the client cannot reach another
    daemon instead of the one this check starts."""
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def run(client, plugin_classes):
    names = set(client.list_mclasses().names)
    missing = sorted(set(plugin_classes) - names)
    if missing:
        raise RuntimeError(f'plugin classes not registered: {missing}')
    client.add_worker(0, 0)
    names = {mclass: [f'{mclass.lower()}{i}' for i in range(count)]
             for mclass, count in plugin_classes.items()}
    for mclass, instances in names.items():
        for name in instances:
            client.create_module('Source', name + '_src', {})
            client.create_module(mclass, name, {})
            client.create_module('Sink', name + '_sink', {})
            client.connect_modules(name + '_src', name)
            client.connect_modules(name, name + '_sink')
    client.resume_all()
    time.sleep(0.3)
    client.pause_all()
    for mclass, instances in names.items():
        for name in instances:
            sent = edge_packets(client, name)
            if sent == 0:
                raise RuntimeError(f'{mclass} ({name}): no packets left the module')
            print(f'  OK: {mclass} ({name}) loaded from the installed tree and moved {sent} packets')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bessd', required=True)
    parser.add_argument('--plugin-dir', required=True)
    parser.add_argument('--grpc-url', help='default: 127.0.0.1 on a free port')
    args = parser.parse_args()
    args.grpc_url = args.grpc_url or f'127.0.0.1:{free_port()}'

    from pybess.bess import BESS

    with tempfile.TemporaryDirectory(prefix='bess-standalone-') as temporary:
        log_path = Path(temporary) / 'bessd.log'
        with open(log_path, 'wb') as log:
            process = subprocess.Popen(
                [args.bessd, '-f', '-skip_root_check', '-m', '0',
                 '-i', str(Path(temporary) / 'bessd.pid'),
                 '-modules', args.plugin_dir, '-grpc_url', args.grpc_url],
                stdout=subprocess.DEVNULL, stderr=log)
        client = BESS()
        try:
            wait_connected(client, process, args.grpc_url, log_path)
            try:
                run(client, PLUGIN_CLASSES)
            except Exception as error:
                time.sleep(0.5)
                raise RuntimeError(f'{error}\nbessd log (tail):\n'
                                   + log_path.read_text(errors='replace')[-4000:]) from None
            return 0
        finally:
            client.disconnect()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
