#!/usr/bin/env python3
# Copyright (c) 2026, Nefeli Networks, Inc.
# All rights reserved. (BSD 3-clause; see LICENSE.)
"""Packet rate of a live bessd while dataplane transactions change its rules.

The live gate MODERNIZATION.md section 31.0 asked for (D-021 amendment 5,
D-025): packets classified by a real ExactMatch module on a running worker
while a controller changes that module's rules through the transaction RPC
at a given rate. It reports, per rate:

  - Mpps through the module (its output gates' counters);
  - the fraction of packets that hit a rule (half the traffic targets live
    sessions, so ~0.5 confirms the rules steer packets);
  - transactions/s achieved, client-side latency p50/p99, BUSY retries.

Pipeline: Source -> RandomUpdate (IPv4 source address over 2 x SESSIONS
values) -> ExactMatch (source address) -> Sink per gate. A session is one
exact rule; each transaction adds the next session's rule and removes the
oldest (two operations), so the live set stays at SESSIONS.

Start bessd first (isolated as you benchmark anything else), e.g.
  omarchy-benchmark --cpu 2,4 --isolate -- sudo env ... bessd -k -f
then run this from another core:
  tools/live_transaction_bench.py --worker-core 2 --rates 0,1000,10000,max
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from pybess.bess import BESS  # noqa: E402
from builtin_pb import control_v2_pb2 as control_v2  # noqa: E402
from builtin_pb import module_msg_pb2 as module_msg  # noqa: E402

BASE = 0x0a000000  # 10.0.0.0
GATES = 4          # rules steer to gates 1..GATES; misses go to gate 0
APPLIED = control_v2.TransactionRecord.OUTCOME_APPLIED
BUSY = control_v2.TransactionRecord.OUTCOME_BUSY


def rule_op(bess, module, session, sessions, erase=False):
    """The rule for `session` (its address cycles over 2 x sessions)."""
    key = module_msg.ExactMatchRuleKey()
    addr = BASE + session % (2 * sessions)
    key.fields.add(value_bin=struct.pack('>I', addr))
    if erase:
        return bess.transaction_op(module + '/rules', key, erase=True)
    return bess.transaction_op(
        module + '/rules', key,
        module_msg.ExactMatchRuleValue(gate=1 + session % GATES))


def apply(bess, ops, stats):
    while True:
        start = time.perf_counter()
        r = bess.apply_transaction(ops)
        stats['latency'].append(time.perf_counter() - start)
        if r.record.outcome == BUSY:
            stats['busy'] += 1
            continue
        if r.record.outcome != APPLIED:
            errors = [op.error for op in r.record.ops if op.error]
            raise RuntimeError('transaction rejected: %s' % errors)
        return


def gate_packets(bess, module):
    info = bess.get_module_info(module)
    return {g.ogate: g.pkts for g in info.ogates}


def build(bess, args):
    bess.pause_all()
    bess.reset_all()
    bess.add_worker(0, args.worker_core)
    src = bess.create_module('Source', 'src', {'pkt_size': args.pkt_size})
    rnd = bess.create_module('RandomUpdate', 'rnd', {'fields': [{
        'offset': 26, 'size': 4,
        'min': BASE, 'max': BASE + 2 * args.sessions - 1}]})
    em = bess.create_module('ExactMatch', 'em',
                            {'fields': [{'offset': 26, 'num_bytes': 4}]})
    bess.run_module_command('em', 'set_default_gate',
                            'ExactMatchCommandSetDefaultGateArg', {'gate': 0})
    bess.connect_modules(src.name, rnd.name)
    bess.connect_modules(rnd.name, em.name)
    for gate in range(GATES + 1):
        sink = bess.create_module('Sink', 'sink%d' % gate)
        bess.connect_modules(em.name, sink.name, gate, 0)
    # Sessions 0 .. SESSIONS-1 live, loaded in transactions of 1000 rules.
    stats = {'latency': [], 'busy': 0}
    for first in range(0, args.sessions, 1000):
        ops = [rule_op(bess, 'em', s, args.sessions)
               for s in range(first, min(first + 1000, args.sessions))]
        apply(bess, ops, stats)
    bess.resume_all()
    time.sleep(1)  # warm up


def measure(bess, args, rate):
    stats = {'latency': [], 'busy': 0}
    nxt = args.next_session
    oldest = nxt - args.sessions
    before = gate_packets(bess, 'em')
    start = time.perf_counter()
    due = start
    done = 0
    while time.perf_counter() - start < args.duration:
        if rate == 0:
            time.sleep(0.01)
            continue
        if rate > 0:
            due += 1.0 / rate
            delay = due - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
        apply(bess, [rule_op(bess, 'em', nxt, args.sessions),
                     rule_op(bess, 'em', oldest, args.sessions, erase=True)],
              stats)
        nxt += 1
        oldest += 1
        done += 1
    elapsed = time.perf_counter() - start
    after = gate_packets(bess, 'em')
    args.next_session = nxt
    pkts = {g: after[g] - before.get(g, 0) for g in after}
    total = sum(pkts.values())
    lat = sorted(stats['latency']) or [0.0]
    return {
        'rate': 'max' if rate < 0 else rate,
        'mpps': total / elapsed / 1e6,
        'hit': 1 - pkts.get(0, 0) / total if total else 0.0,
        'tx_per_s': done / elapsed,
        'p50_us': lat[len(lat) // 2] * 1e6,
        'p99_us': lat[min(len(lat) - 1, int(len(lat) * 0.99))] * 1e6,
        'busy': stats['busy'],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--grpc-url', default='localhost:10514')
    parser.add_argument('--worker-core', type=int, default=0)
    parser.add_argument('--sessions', type=int, default=100000)
    parser.add_argument('--pkt-size', type=int, default=60)
    parser.add_argument('--duration', type=float, default=5.0)
    parser.add_argument('--rates', default='0,1000,10000,max',
                        help='transactions/s; max = as fast as possible')
    parser.add_argument('--rounds', type=int, default=1,
                        help='repeat the rate list, interleaved')
    args = parser.parse_args()
    rates = [-1 if r == 'max' else int(r) for r in args.rates.split(',')]

    bess = BESS()
    bess.connect(grpc_url=args.grpc_url)
    build(bess, args)
    args.next_session = args.sessions
    print('%8s %8s %6s %10s %9s %9s %6s' %
          ('rate', 'Mpps', 'hit', 'tx/s', 'p50 us', 'p99 us', 'busy'))
    for _ in range(args.rounds):
        for rate in rates:
            r = measure(bess, args, rate)
            print('%8s %8.2f %6.2f %10.0f %9.0f %9.0f %6d' %
                  (r['rate'], r['mpps'], r['hit'], r['tx_per_s'],
                   r['p50_us'], r['p99_us'], r['busy']))
    bess.pause_all()
    bess.reset_all()


if __name__ == '__main__':
    main()
