#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Phase L4 Batch Size Sweep Tool.

Empirically sweeps packet burst / batch sizes (16, 32, 64, 128, 256) across
Google Benchmark executables or BESS microbenchmarks to measure throughput,
per-packet latency, and cache scaling.

Usage:
  tools/batch_sweep.py <benchmark_executable> [--wrap "command"] [--filter REGEX]
                       [--bursts 16,32,64,128,256] [--json OUT.json]
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(
        description="Phase L4 Batch Size Sweep Tool: sweeps burst sizes and maps throughput vs latency"
    )
    parser.add_argument("binary", help="Path to Google Benchmark binary")
    parser.add_argument(
        "--wrap",
        default="taskset -c 2",
        help="Command prefix for CPU isolation (e.g. 'omarchy-benchmark --cpu 2 --isolate --')",
    )
    parser.add_argument(
        "--filter",
        default="",
        help="Benchmark filter regex (e.g. 'BM_K38RangeLookupBatch|BM_RouteDomain')",
    )
    parser.add_argument(
        "--bursts",
        default="16,32,64,128,256",
        help="Comma-separated burst sizes to sweep (default: 16,32,64,128,256)",
    )
    parser.add_argument(
        "--min-time", default="0.05s", help="Minimum time per benchmark run"
    )
    parser.add_argument(
        "--json", dest="json_out", default=None, help="Save raw results as JSON"
    )
    return parser.parse_args()


def run_benchmark(binary, burst, filter_regex, wrap_cmd, min_time):
    # Pass filter matching this burst if parameterized, or general filter
    effective_filter = filter_regex
    if filter_regex:
        effective_filter = f"({filter_regex})(/.*)?/{burst}($|/)"
    else:
        effective_filter = f".*/{burst}($|/)"

    cmd = []
    if wrap_cmd:
        cmd.extend(shlex.split(wrap_cmd))
    cmd.append(os.path.abspath(binary))
    cmd.extend(
        [
            f"--benchmark_filter={effective_filter}",
            f"--benchmark_min_time={min_time}",
            "--benchmark_format=json",
        ]
    )

    env = os.environ.copy()
    dpdk_lib = "/home/krsna1729/Projects/bess/deps/dpdk-25.11.3/install/lib"
    if "LD_LIBRARY_PATH" in env:
        env["LD_LIBRARY_PATH"] = f"{dpdk_lib}:{env['LD_LIBRARY_PATH']}"
    else:
        env["LD_LIBRARY_PATH"] = dpdk_lib

    proc = subprocess.run(
        cmd, capture_output=True, text=True, env=env, timeout=120
    )
    if proc.returncode != 0:
        # Fallback to general output without json filter
        return None

    try:
        data = json.loads(proc.stdout)
        return data.get("benchmarks", [])
    except Exception:
        return []


def format_table(results):
    print("\n" + "=" * 78)
    print(
        f"{'Benchmark Name':<32} {'Burst':<8} {'Time (ns)':<12} {'Mpps / M/s':<14} {'Speedup vs 32':<12}"
    )
    print("=" * 78)

    # Group by benchmark base name
    by_name = {}
    for r in results:
        base = r["name"].rsplit("/", 1)[0]
        by_name.setdefault(base, []).append(r)

    for base, entries in by_name.items():
        # Find baseline time for burst 32
        base_32 = next((e for e in entries if e["burst"] == 32), None)
        base_time = base_32["real_time"] if base_32 else None

        for e in sorted(entries, key=lambda x: x["burst"]):
            burst = e["burst"]
            real_time = e["real_time"]
            mpps = (
                e.get("items_per_second", 0) / 1e6
                if "items_per_second" in e
                else 0
            )

            if base_time and base_time > 0:
                speedup = base_time / real_time
                speedup_str = f"{speedup:.2f}x"
            else:
                speedup_str = "1.00x (base)" if burst == 32 else "-"

            name_disp = (base[:30] + "..") if len(base) > 32 else base
            print(
                f"{name_disp:<32} {burst:<8} {real_time:<12.2f} {mpps:<14.2f} {speedup_str:<12}"
            )
        print("-" * 78)


def main():
    args = parse_args()
    if not os.path.exists(args.binary):
        print(f"Error: binary '{args.binary}' not found", file=sys.stderr)
        sys.exit(1)

    bursts = [int(b.strip()) for b in args.bursts.split(",") if b.strip()]
    all_results = []

    print(f"Starting Phase L4 Burst Size Sweep on {args.binary}")
    print(f"Bursts: {bursts}")
    print(f"Wrapper: '{args.wrap}'\n")

    for burst in bursts:
        sys.stdout.write(f"  Measuring burst size {burst:>3}... ")
        sys.stdout.flush()
        benchmarks = run_benchmark(
            args.binary, burst, args.filter, args.wrap, args.min_time
        )
        if benchmarks:
            for b in benchmarks:
                b["burst"] = burst
                all_results.append(b)
            print(f"done ({len(benchmarks)} benchmarks)")
        else:
            print("no matching tests")

    if all_results:
        format_table(all_results)

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(all_results, f, indent=2)
        print(f"Results saved to {args.json_out}")


if __name__ == "__main__":
    main()
