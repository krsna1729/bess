#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS metrics in the Prometheus text exposition format (M25).

    bess_prometheus.py [--address localhost:10514] [--listen :9600]

Without --listen it prints one scrape and exits; with it, it serves /metrics
over HTTP and asks bessd (ListMetrics) on every scrape. The exporter lives
outside the daemon: nothing in bessd knows this format (roadmap M25).
"""

import argparse
import http.server
import os
import sys

import grpc

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pybess.bess import BESS  # noqa: E402  (sets up builtin_pb on the path)
from builtin_pb import control_v2_pb2 as v2  # noqa: E402
from builtin_pb import control_v2_pb2_grpc as v2_grpc  # noqa: E402

_KIND = {v2.MetricSample.KIND_COUNTER: 'counter', v2.MetricSample.KIND_GAUGE: 'gauge'}


def _escape_label(value):
    return value.replace('\\', '\\\\').replace('\n', '\\n').replace('"', '\\"')


def _escape_help(value):
    return value.replace('\\', '\\\\').replace('\n', '\\n')


def exposition(response):
    """The text format for a ListMetricsResponse (samples sorted by name)."""
    lines = []
    last = None
    for s in response.samples:
        if s.name != last:
            lines.append('# HELP %s %s' % (s.name, _escape_help(s.help)))
            lines.append('# TYPE %s %s' % (s.name, _KIND.get(s.kind, 'untyped')))
            last = s.name
        labels = ','.join('%s="%s"' % (k, _escape_label(v)) for k, v in sorted(s.labels.items()))
        lines.append('%s%s %s' % (s.name, '{%s}' % labels if labels else '', repr(float(s.value))))
    lines.append('# HELP bess_daemon_epoch Daemon epoch: counters restart when it changes')
    lines.append('# TYPE bess_daemon_epoch gauge')
    lines.append('bess_daemon_epoch %d' % response.daemon_epoch)
    return '\n'.join(lines) + '\n'


def scrape(address, timeout=5.0):
    channel = grpc.insecure_channel(address)
    try:
        return exposition(v2_grpc.ControlStub(channel).ListMetrics(v2.ListMetricsRequest(),
                                                                   timeout=timeout))
    finally:
        channel.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--address', default=BESS.DEF_GRPC_URL)
    parser.add_argument('--listen', help='[host]:port to serve /metrics on')
    args = parser.parse_args()
    if not args.listen:
        sys.stdout.write(scrape(args.address))
        return 0
    host, _, port = args.listen.rpartition(':')

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != '/metrics':
                self.send_error(404)
                return
            try:
                body = scrape(args.address).encode()
            except grpc.RpcError as e:
                self.send_error(503, 'bessd: %s' % e.code())
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; version=0.0.4')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    http.server.ThreadingHTTPServer((host, int(port)), Handler).serve_forever()


if __name__ == '__main__':
    sys.exit(main())
