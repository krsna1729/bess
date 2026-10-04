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

def ipv4_udp_frame(dst, ttl=64):
    """60 bytes: Ethernet, IPv4 (192.168.0.1 -> `dst`, a dotted quad), UDP."""
    f = bytearray(udp_frame(53))
    f[22] = ttl
    f[26:30] = bytes([192, 168, 0, 1])
    f[30:34] = bytes(int(b) for b in dst.split('.'))
    f[24:26] = b'\x00\x00'
    s = sum((f[i] << 8) | f[i + 1] for i in range(14, 34, 2))
    s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    f[24], f[25] = (~s >> 8) & 0xff, ~s & 0xff
    return bytes(f)


def udp_frame(dst_port):
    """60 bytes: Ethernet, IPv4 (10.0.0.1 -> 192.0.2.1), UDP to `dst_port`."""
    f = bytearray(60)
    f[12] = 0x08                            # IPv4
    f[14], f[17], f[22], f[23] = 0x45, 28, 64, 17
    f[26], f[29] = 10, 1
    f[30], f[32], f[33] = 192, 2, 1
    f[34], f[35] = 0x03, 0xe8               # source port 1000
    f[36], f[37] = dst_port >> 8, dst_port & 0xff
    f[39] = 8
    return bytes(f)


# (module class, instance name, packet template or None for Source's own,
#  the output gate those packets must leave on). Every Init takes EmptyArg.
# Reference appliances (M24): instances, then module commands each must
# answer without error (the direct path, no graph).
APPLIANCES = [
    # R1: 172.16.1.1 arriving on igate 0 (VRF 1) leaves by interface 1 (gate 0).
    ('RouterAppliance', 'router0', ipv4_udp_frame('172.16.1.1'), 0),
]
APPLIANCE_COMMANDS = [
    ('router0', 'self_test'),
]

INSTANCES = [
    ('StandalonePass', 'pass0', None, 0),
    ('StandaloneMacSwap', 'macswap0', None, 0),
    ('StandaloneRangeGate', 'rangegate0', udp_frame(1500), 1),  # in [1000, 2000]
    ('StandaloneFlowCount', 'flowcount0', None, 0),  # flows it already knows
    # The second appliance looks up the first's policy graph; one sees a
    # classifier hit (gate 1), the other the default action (gate 0).
    ('StandaloneAppliance', 'appliance0', udp_frame(1500), 1),
    ('StandaloneAppliance', 'appliance1', udp_frame(53), 0),
    # Gate 1 only after a request round trip through bessd's maintenance loop.
    ('StandaloneRequests', 'requests0', None, 1),
]


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


def run(client, instances, commands=()):
    names = set(client.list_mclasses().names)
    missing = sorted({mclass for mclass, *_ in instances} - names)
    if missing:
        raise RuntimeError(f'plugin classes not registered: {missing}')
    client.add_worker(0, 0)
    for mclass, name, template, gate in instances:
        client.create_module('Source', name + '_src', {})
        client.create_module(mclass, name, {})
        client.create_module('Sink', name + '_sink', {})
        if template is not None:
            client.create_module('Rewrite', name + '_rw', {'templates': [template]})
            client.connect_modules(name + '_src', name + '_rw')
            client.connect_modules(name + '_rw', name)
        else:
            client.connect_modules(name + '_src', name)
        client.connect_modules(name, name + '_sink', gate)
    client.resume_all()
    time.sleep(0.3)
    client.pause_all()
    for mclass, name, _, gate in instances:
        sent = edge_packets(client, name, gate)
        if sent == 0:
            raise RuntimeError(f'{mclass} ({name}): no packets left on gate {gate}')
        print(f'  OK: {mclass} ({name}) loaded from the installed tree and moved {sent} '
              f'packets on gate {gate}')
    for name, command in commands:
        client.run_module_command(name, command, 'EmptyArg', {})  # raises on failure
        print(f'  OK: {name}.{command}()')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bessd', required=True)
    parser.add_argument('--plugin-dir', required=True)
    parser.add_argument('--grpc-url', help='default: 127.0.0.1 on a free port')
    parser.add_argument('--set', choices=('standalone', 'appliances'), default='standalone',
                        help='the conformance plugins (examples/standalone_plugin) or the '
                             'reference appliances (examples/appliances)')
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
                if args.set == 'appliances':
                    run(client, APPLIANCES, APPLIANCE_COMMANDS)
                else:
                    run(client, INSTANCES)
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
