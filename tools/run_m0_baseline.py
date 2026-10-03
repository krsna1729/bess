#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Rerun the M0 paired baseline: f4fdab03 (A, pre-modernization) vs B (this tree).

One command (builds, measures every group under CPU isolation, summarizes):
  python3 tools/run_m0_baseline.py all --outdir .scratch/m0/run

`all` runs these steps, which can also be run one at a time:
  flock LOCK python3 tools/run_m0_baseline.py build
  omarchy-benchmark --isolate --cpu 2 --diagnose -- \\
      flock LOCK python3 tools/run_m0_baseline.py measure --group <g> --outdir DIR
  python3 tools/run_m0_baseline.py summarize DIR/*.json --output OUT.json

Layout: A is the detached worktree ../base at f4fdab03 (override: M0_BASE), B is
this checkout; both have build/perf-release configured with the same compiler, -O3
(buildtype=release), -Dcpu=x86-64-v3 and the same DPDK install (the main
checkout's; override: M0_DPDK_PKGCONFIG; the lock: M0_LOCK). `measure`
refuses to start unless A is at f4fdab03, every
compared benchmark source is byte-identical apart from D-059's logging include
(route_bench.cc is the one documented API-adapted exception;
route_domain_bench.cc is rewritten and not compared), the compile flags match
and both trees are fully built.

Each case is measured in ABBA order (A B B A, repeated), every run writing its
own --benchmark_out JSON so daemon/EAL log lines cannot corrupt the data, and
compared per adjacent pair with tools/ab_bench.py's rule: a difference is
called only when the median paired ratio is outside +/-3% and at least 3/4 of
the pairs agree on its sign. --rounds R gives R pairs (R runs per side).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
# The main checkout (worktrees share its .git): the build lock and DPDK live there.
MAIN = Path(subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute",
                            "--git-common-dir"], capture_output=True, text=True,
                           check=True).stdout.strip()).parent
BASE = Path(os.environ.get("M0_BASE", ROOT.parent / "base"))
REV_A = "f4fdab03c10f9f0b39147a59230d2a678e6050a8"
LOCK = os.environ.get("M0_LOCK", str(MAIN / ".scratch/build.lock"))
PKG = os.environ.get("M0_DPDK_PKGCONFIG", str(MAIN / "deps/dpdk-25.11.3/install/lib/pkgconfig"))
BUILD = "build/perf-release"
SLOT_SOURCE = ROOT / "tools/m0_slot_table_bench.cc"
SLOT_TARGET = "m0_slot_table_bench"
NOISE = 0.03

# Benchmark sources that must be byte-identical between A and B.
IDENTICAL = {
    "packet_bench": "core/packet_bench.cc",
    "packet_checksum_bench": "core/packet_checksum_bench.cc",
    "packet_tx_checksum_bench": "core/packet_tx_checksum_bench.cc",
    "utils_checksum_bench": "core/utils/checksum_bench.cc",
    "classifier_bench": "core/classifier/classifier_bench.cc",
    "classifier_typed_bench": "core/classifier/typed_exact_bench.cc",
    "classifier_masked_bench": "core/classifier/masked_bench.cc",
    "classifier_range_backend_bench": "core/classifier/range_backend_bench.cc",
    "modules_exact_match_bench": "core/modules/exact_match_bench.cc",
    "modules_wildcard_match_bench": "core/modules/wildcard_match_bench.cc",
    "meter_bench": "core/meter/meter_bench.cc",
    "stats_bench": "core/stats/stats_bench.cc",
    "fib_bench": "core/fib_bench.cc",
    "rcu_bench": "core/rcu/rcu_bench.cc",
    "object_table_bench": "core/dataplane/object_table_bench.cc",
    "transaction_bench": "core/dataplane/transaction_bench.cc",
    "ring_bench": "core/ring_bench.cc",
    "modules_exact_match_transaction_bench": "core/modules/exact_match_transaction_bench.cc",
}
ADAPTED = {"route_bench": "core/route/route_bench.cc"}
# The supplemental SlotTable/StrongId benchmark compiles one source against
# both trees; the headers it measures must be identical too.
SLOT_HEADERS = ("core/dataplane/slot_table.h", "core/dataplane/strong_id.h")

# (group, target, filter, metric, higher_is_better). metric is real_time or a
# user counter for rows whose time is fixed by MinTime.
CASES = [
    ("packet", "packet_bench",
     r"^BM_PacketAllocFree$|^BM_PacketCursorPositionedRead/width:4/offset:(14|34)/batch:1$"
     r"|^BM_PacketCursorSequentialRead/batch:8$|^BM_PacketCheckedPrepend/bytes:14/shape:0/batch:1$"
     r"|^BM_PacketCheckedAppend/bytes:8/shape:0/batch:1$|^BM_PacketCheckedRemovePrefix/bytes:14/shape:0/batch:1$"
     r"|^BM_PayloadWriteability/storage:0$|^BM_EnsureWritableUniqueMultisegment/bytes:256$"
     r"|^BM_EnsureWritableSharedCow/bytes:256$|^BM_EnsureLinearNativeTwoSegment$"
     r"|^BM_EnsureContiguousCrossesTwoSegments$", "real_time", False),
    ("packet", "packet_checksum_bench",
     r"^BM_(RawBess|ValidatedBess)Contiguous/packet_bytes:(64|1500)/protocol:6/checksum_intent:0$"
     r"|^BM_ComputeChecksums/packet_bytes:1500/shape:0/protocol:6/checksum_intent:0/cold_rotation:0$",
     "real_time", False),
    ("packet", "packet_tx_checksum_bench",
     r"^BenchmarkTxFinalization/variant:(0|1)/protocol:0/packet_bytes:1500/batch:32/shape:0$"
     r"|^BenchmarkQueueOutEgress/packet_bytes:1500/protocol:0/batch:32/profile:(0|1)$",
     "real_time", False),
    ("packet", "utils_checksum_bench",
     r"^ChecksumFixture/BmGenericChecksum(Dpdk|Bess)/1024$|^ChecksumFixture/BmIncrementalUpdate16$"
     r"|^ChecksumFixture/BmSrcIpPortUpdateBess$", "real_time", False),
    ("classify", "classifier_bench",
     r"^BM_(DirectTypedLookup|TypedTableLookup|Cuckoo_TypedBatch_Hit|Small_TypedBatch_Hit"
     r"|RteHashData_Bulk_Hit)/(1|32)$", "real_time", False),
    ("classify", "classifier_typed_bench",
     r"^BM_K34_Cuckoo_(Direct|Table|Runtime)_K8_R4/16/8/(0|1|2|3)$", "real_time", False),
    ("classify", "classifier_masked_bench",
     r"^BM_Masked_(Legacy|Substrate)/8/4/16/75/1/8/0$", "real_time", False),
    ("classify", "classifier_range_backend_bench",
     r"^BM_(K38RangeLookupBatch|CartesianTernaryLookupBatch)/8$", "real_time", False),
    ("classify", "modules_exact_match_bench", r"^BM_(Legacy|New)EndToEnd/0/8/2$", "real_time", False),
    ("classify", "modules_wildcard_match_bench", r"^BM_Wm_(Legacy|Module)/4/8/0/0$", "real_time", False),
    ("state", "meter_bench", r"^BM_SingleState/(0|1)$|^BM_BatchMeterSet/1024/(0|1)$", "real_time", False),
    ("state", "stats_bench", r"^BM_(AccountCell|AccountUpdate|SnapshotCounters/3)$", "real_time", False),
    ("state", "fib_bench", r"^BM_Lookup(LpmVec|Fib)/routes:1024$", "real_time", False),
    ("state", "route_bench",
     r"^BM_LookupRouter/(1024|65536)$|^BM_LookupRouteTable/1024/0$|^BM_UpdateInPlace/1024$"
     r"|^BM_NextHopUpdate/1024$", "real_time", False),
    ("state", "rcu_bench", r"^BM_(RcuPtrRead|GracePeriodLatency|PublishWithIdleReader)$", "real_time", False),
    ("state", "object_table_bench",
     r"^BM_ObjectTableLookup<(16|64)>/10240/0$|^BM_ObjectTableBatchLookup<16>/32$"
     r"|^BM_ObjectTableBuild<64>/10240$", "real_time", False),
    ("state", "transaction_bench", r"^BM_SingleRule/(0|1)$|^BM_SessionEstablishRelease/1024$",
     "real_time", False),
    ("state", "modules_exact_match_transaction_bench", r"^BM_ModuleClassify/1000/0$", "real_time", False),
    ("state", SLOT_TARGET, r"^BM_(SlotTable(Lookup|Publish)|StrongIdCompare)$", "real_time", False),
    # Two-thread rows: run with two isolated CPUs (omarchy-benchmark --cpu 2,4).
    ("threads", "transaction_bench", r"^BM_SessionWithOnlineReader/real_time$", "real_time", False),
    ("threads", "transaction_bench", r"^BM_LookupsUnderTransactions/1/10000/min_time:2.000/real_time$",
     "Mlookups_per_reader", True),
    ("threads", "modules_exact_match_transaction_bench",
     r"^BM_ClassifyUnderTransactions/1/10000/min_time:2.000/real_time$", "Mpps_per_reader", True),
    ("threads", "ring_bench", r"^BM_RingRte(Mpsc|RtsSc|HtsSc)/1$", "real_time", False),
    ("threads", "stats_bench", r"^BM_Threads(WorkerLocalUpdate|SharedRmw)/real_time/threads:2$",
     "real_time", False),
    ("threads", "meter_bench", r"^BM_Threads(OneSharedMeter|ExclusiveMeterEach)/real_time/threads:2$",
     "real_time", False),
]
GROUP_CPUS = {"packet": "2", "classify": "2", "state": "2", "threads": "2,4"}
TARGETS = sorted({t for _, t, *_ in CASES} - {SLOT_TARGET})


def run(cmd, **kw):
    p = subprocess.run([str(c) for c in cmd], text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, **kw)
    if p.returncode:
        sys.exit(f"command failed ({p.returncode}): {shlex.join(map(str, cmd))}\n{p.stdout[-4000:]}")
    return p.stdout


def git(root, *args):
    return run(["git", "-C", root, *args]).strip()


def blob(root, rev, path):
    return git(root, "rev-parse", f"{rev}:{path}")


# D-059 respelled every glog include as "utils/logging.h"; a source that differs
# only by that line (wherever the include block puts it) is the same benchmark.
LOGGING_INCLUDES = {"#include <glog/logging.h>", '#include "utils/logging.h"'}


def same_source(rev_a, rev_b, path):
    """'identical', 'identical except the D-059 logging include', or None."""
    a, b = blob(BASE, rev_a, path), blob(ROOT, rev_b, path)
    if a == b:
        return "identical"
    def body(root, rev):
        text = git(root, "show", f"{rev}:{path}")
        return [line for line in text.splitlines() if line.strip() not in LOGGING_INCLUDES]
    if body(BASE, rev_a) == body(ROOT, rev_b):
        return "identical except the D-059 logging include"
    return None


def compile_flags(root):
    """route_bench's compile line without include paths and file names."""
    line = run(["ninja", "-C", root / BUILD, "-t", "commands", "core/route_bench"]).splitlines()
    cmd = next(c for c in line if "-c ../../core/route/route_bench.cc" in c)
    args = shlex.split(cmd)
    # Drop a ccache launcher; compare the compiler by what it is ("c++" and "g++"
    # may be one GCC), then the flags.
    if args and os.path.basename(args[0]) == "ccache":
        args = args[1:]
    compiler = run([args[0], "--version"]).splitlines()[0]
    name = os.path.basename(args[0]) + " "
    compiler = compiler[len(name):] if compiler.startswith(name) else compiler  # GCC echoes argv[0]
    keep = [compiler] + [a for a in args[1:] if not a.startswith(("-I", "-isystem", "-M", "core/"))
                         and not a.endswith((".o", ".d", ".cc"))
                         and a not in ("-o", "-c", "-MQ", "-MF")]
    return keep


def source_proof():
    rev_a, rev_b = git(BASE, "rev-parse", "HEAD"), git(ROOT, "rev-parse", "HEAD")
    if rev_a != REV_A:
        sys.exit(f"A checkout {BASE} is at {rev_a}, expected {REV_A}")
    if git(ROOT, "status", "--porcelain", "--untracked-files=no", "--", "core"):
        sys.exit("B has uncommitted changes under core/; measure a committed tree")
    identical = {}
    for target, path in IDENTICAL.items():
        how = same_source(rev_a, rev_b, path)
        if how is None:
            sys.exit(f"benchmark source differs: {path}")
        identical[target] = {"path": path, "blob_A": blob(BASE, rev_a, path),
                             "blob_B": blob(ROOT, rev_b, path), "comparison": how}
    adapted = {t: {"path": p, "blob_A": blob(BASE, rev_a, p), "blob_B": blob(ROOT, rev_b, p)}
               for t, p in ADAPTED.items()}
    headers = {}
    for path in SLOT_HEADERS:
        how = same_source(rev_a, rev_b, path)
        if how is None:
            sys.exit(f"{path} differs; the supplemental benchmark is not comparable")
        headers[path] = {"blob_A": blob(BASE, rev_a, path), "blob_B": blob(ROOT, rev_b, path),
                         "comparison": how}
    flags_a, flags_b = compile_flags(BASE), compile_flags(ROOT)
    if flags_a != flags_b:
        sys.exit(f"compile flags differ:\nA {flags_a}\nB {flags_b}")
    return {
        "A": {"tree": str(BASE), "commit": rev_a},
        "B": {"tree": str(ROOT), "commit": rev_b},
        "identical_sources": identical,
        "adapted_sources": adapted,
        "not_compared": {"core/route/route_domain_bench.cc": "rewritten after f4fdab03 (M6); "
                         "workload differs", "core/rcu/grace_period_bench.cc": "custom "
                         "non-Google-Benchmark harness needing 5 isolated CPUs and ~75 s per run; "
                         "rcu_bench BM_GracePeriodLatency covers grace-period latency"},
        "supplemental": {"source": str(SLOT_SOURCE.relative_to(ROOT)),
                         "sha256": hashlib.sha256(SLOT_SOURCE.read_bytes()).hexdigest(),
                         "header_blobs": headers},
        "compile_flags": flags_a,
    }


def read(path, default="n/a"):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def own_cpuset():
    rel = read("/proc/self/cgroup").splitlines()[-1].split(":", 2)[2]
    cg = Path("/sys/fs/cgroup") / rel.lstrip("/")
    for d in (cg, *cg.parents):
        part = read(d / "cpuset.cpus.partition", "")
        if part.startswith("isolated"):
            return {"cgroup": str(d), "partition": part, "cpus": read(d / "cpuset.cpus.effective")}
    return {"cgroup": str(cg), "partition": "none", "cpus": read(cg / "cpuset.cpus.effective")}


def cpu_list(spec):
    cpus = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        cpus += range(int(lo), int(hi or lo) + 1)
    return cpus


def metadata(cpus):
    model = next((x.split(":", 1)[1].strip() for x in run(["lscpu"]).splitlines()
                  if x.startswith("Model name:")), "unknown")
    per_cpu = {c: {"governor": read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_governor"),
                   "epp": read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/energy_performance_preference"),
                   "max_khz": read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_max_freq")}
               for c in cpu_list(cpus)}
    return {
        "captured_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host_kernel": platform.release(),
        "compiler": run(["g++", "--version"]).splitlines()[0],
        "cpu_model": model,
        "online_cpus": read("/sys/devices/system/cpu/online"),
        "benchmark_cpus": cpus,
        "cpu_state": per_cpu,
        "no_turbo": read("/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "isolation": own_cpuset(),
        "dpdk_pkg_config": PKG,
        "dpdk_version": run(["env", f"PKG_CONFIG_PATH={PKG}", "pkg-config", "--modversion",
                             "libdpdk"]).strip(),
        "meson_options": "buildtype=release (-O3), cpu=x86-64-v3",
    }


def cmd_build(o):
    flags = ["-std=c++23", "-O3", "-march=x86-64-v3", "-D_GLIBCXX_ASSERTIONS=1",
             "-DGLOG_USE_GLOG_EXPORT", "-pthread"]
    for root in (BASE, ROOT):
        if not (root / BUILD).exists():
            run(["meson", "setup", root / BUILD, root, "-Dbuildtype=release",
                 "-Dcpu=x86-64-v3", f"-Dpkg_config_path={PKG}"], env={**os.environ, "CC": "gcc",
                                                                       "CXX": "g++"})
        print(run(["ninja", "-C", root / BUILD, f"-j{o.jobs}", *[f"core/{t}" for t in TARGETS]])
              .splitlines()[-1])
        libs = run(["pkg-config", "--libs", "benchmark", "libglog"]).split()
        run(["g++", *flags, f"-I{root / 'core'}", SLOT_SOURCE, *libs, "-o",
             root / BUILD / SLOT_TARGET])
    print("built", len(TARGETS) + 1, "targets in", BASE, "and", ROOT)


def check_built():
    for root in (BASE, ROOT):
        out = run(["ninja", "-C", root / BUILD, "-n", *[f"core/{t}" for t in TARGETS]])
        if "no work to do" not in out:
            sys.exit(f"{root / BUILD} is stale; run the build step first")
        slot = root / BUILD / SLOT_TARGET
        if not slot.exists() or slot.stat().st_mtime < SLOT_SOURCE.stat().st_mtime:
            sys.exit(f"{slot} missing or stale; run the build step first")


def binary(root, target):
    return root / BUILD / (target if target == SLOT_TARGET else f"core/{target}")


def one_run(path, filt, min_time, cpus, out, env):
    # --data, not --as: on this host the EAL reserves >32 GB of PROT_NONE address
    # space (1 GB hugepage memseg lists); RLIMIT_DATA still caps a runaway heap.
    cmd = ["timeout", "120", "prlimit", "--data=8589934592", "taskset", "-c", cpus, path,
           f"--benchmark_filter={filt}", f"--benchmark_min_time={min_time}",
           f"--benchmark_out={out}", "--benchmark_out_format=json"]
    p = subprocess.run([str(c) for c in cmd], text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, env=env)
    out.with_suffix(".log").write_text(p.stdout)
    if p.returncode:
        sys.exit(f"benchmark failed ({p.returncode}): {shlex.join(map(str, cmd))}\n{p.stdout[-3000:]}")
    data = json.loads(out.read_text())
    return {b["name"]: b for b in data["benchmarks"] if b.get("run_type", "iteration") == "iteration"}


def compare(a_runs, b_runs, metric, higher):
    rows = []
    for name in a_runs[0]:
        if not all(name in r for r in a_runs + b_runs):
            continue
        a = [r[name][metric] for r in a_runs]
        b = [r[name][metric] for r in b_runs]
        ratios = [y / x for x, y in zip(a, b)]
        med = statistics.median(ratios)
        better = sum((r > 1) if higher else (r < 1) for r in ratios)
        agree = max(better, len(ratios) - better)
        verdict = "no clear difference"
        if abs(med - 1) > NOISE and agree >= 0.75 * len(ratios):
            verdict = ("B better" if (med > 1) == higher else "B worse") + f" ({100 * (med - 1):+.1f}% {metric})"
        counters = {}
        for k, v in a_runs[0][name].items():
            if isinstance(v, (int, float)) and k not in (
                    "real_time", "cpu_time", "iterations", "repetitions", "repetition_index",
                    "threads", "per_family_instance_index", "family_index"):
                counters[k] = {"A": statistics.median(r[name][k] for r in a_runs),
                               "B": statistics.median(r[name].get(k, float("nan")) for r in b_runs)}
        rows.append({"name": name, "metric": metric, "higher_is_better": higher,
                     "unit": a_runs[0][name].get("time_unit") if metric == "real_time" else metric,
                     "A_median": statistics.median(a), "B_median": statistics.median(b),
                     "paired_ratio_median": med, "paired_ratio_min": min(ratios),
                     "paired_ratio_max": max(ratios), "pairs_B_better": better,
                     "pairs": len(ratios), "verdict": verdict, "counters": counters})
    return rows


def sizes(target):
    out = {}
    for side, root in (("A", BASE), ("B", ROOT)):
        path = binary(root, target)
        text, data, bss = run(["size", path]).splitlines()[1].split()[:3]
        out[side] = {"file_bytes": path.stat().st_size, "text": int(text), "data": int(data),
                     "bss": int(bss)}
    return out


def cmd_measure(o):
    sys.path.insert(0, str(ROOT / "tools"))
    import ab_bench  # the busy-host pre-flight check is shared with ab_bench.py
    cpus = o.cpus or GROUP_CPUS[o.group]
    proof = source_proof()
    check_built()
    meta = metadata(cpus)
    iso = meta["isolation"]
    isolated = iso["partition"].startswith("isolated") and cpu_list(iso["cpus"]) == cpu_list(cpus)
    if not isolated and not o.allow_busy:
        busy = ab_bench.busy_processes()
        if busy:
            for pid, frac, cmd in busy:
                print(f"busy: pid {pid} at {100 * frac:.0f}% CPU: {cmd}", file=sys.stderr)
            sys.exit("refusing to benchmark on a busy, non-isolated machine "
                     "(run under omarchy-benchmark --isolate, or pass --allow-busy)")
    meta.update(rounds=o.rounds, min_time=o.min_time, group=o.group,
                cpu_isolated=isolated, allow_busy=o.allow_busy,
                provisional=o.allow_busy and not isolated)
    outdir = Path(o.outdir)
    raw = outdir / "raw" / o.group
    raw.mkdir(parents=True, exist_ok=True)
    report = {"source_proof": proof, "metadata": meta, "cases": [], "complete": False}
    out_json = outdir / f"{o.group}.json"
    dpdk_lib = Path(PKG).parent.parent / "lib"
    env = {**os.environ, "LD_LIBRARY_PATH": f"{dpdk_lib / 'x86_64-linux-gnu'}:{dpdk_lib}"}
    started = time.monotonic()
    for group, target, filt, metric, higher in CASES:
        if group != o.group or (o.only and target not in o.only):
            continue
        t0 = time.monotonic()
        runs = {"A": [], "B": []}
        order = ["A", "B", "B", "A"] * (o.rounds // 2)
        for i, side in enumerate(order):
            path = binary(BASE if side == "A" else ROOT, target)
            out = raw / f"{target}.{len(runs[side]) + 1:02d}{side}.{i:02d}.json"
            runs[side].append(one_run(path, filt, o.min_time, cpus, out, env))
            print(".", end="", flush=True, file=sys.stderr)
        rows = compare(runs["A"], runs["B"], metric, higher)
        if not rows:
            sys.exit(f"{target}: filter matched nothing: {filt}")
        report["cases"].append({"target": target, "filter": filt,
                                "source": "adapted" if target in ADAPTED else
                                ("supplemental" if target == SLOT_TARGET else "identical"),
                                "sizes": sizes(target), "rows": rows,
                                "seconds": round(time.monotonic() - t0, 1)})
        out_json.write_text(json.dumps(report, indent=1) + "\n")
        print(f"\n{target}: {len(rows)} rows, {time.monotonic() - t0:.0f} s", file=sys.stderr)
    report["complete"] = True
    report["metadata"]["seconds"] = round(time.monotonic() - started, 1)
    out_json.write_text(json.dumps(report, indent=1) + "\n")
    print(f"wrote {out_json}")


def cmd_summarize(o):
    groups = [json.loads(Path(p).read_text()) for p in o.inputs]
    for g in groups:
        if not g.get("complete"):
            sys.exit("incomplete measurement input")
    proof = groups[0]["source_proof"]
    if any(g["source_proof"]["B"]["commit"] != proof["B"]["commit"] for g in groups):
        sys.exit("inputs measured different B commits")
    # /2: per-source blob_A/blob_B/comparison (D-059 include respelling) and the
    # compiler identity first in compile_flags; /1 had one blob and flags only.
    out = {"schema": "bess-m0-baseline/2", "source_proof": proof,
           "groups": {g["metadata"]["group"]: {"metadata": g["metadata"], "cases": [
               {k: v for k, v in c.items()} for c in g["cases"]]} for g in groups}}
    Path(o.output).parent.mkdir(parents=True, exist_ok=True)
    Path(o.output).write_text(json.dumps(out, indent=1) + "\n")
    for g in groups:
        m = g["metadata"]
        print(f"\n## {m['group']} (CPUs {m['benchmark_cpus']}, isolated={m['cpu_isolated']}, "
              f"{m['rounds']} pairs)\n| benchmark | metric | A | B | B/A median (min..max) | B better | verdict |")
        print("|---|---|---|---|---|---|---|")
        for c in g["cases"]:
            for r in c["rows"]:
                print(f"| {c['target']}:{r['name']} | {r['unit']} | {r['A_median']:.4g} | "
                      f"{r['B_median']:.4g} | {r['paired_ratio_median']:.3f} "
                      f"({r['paired_ratio_min']:.3f}..{r['paired_ratio_max']:.3f}) | "
                      f"{r['pairs_B_better']}/{r['pairs']} | {r['verdict']} |")
    print(f"wrote {o.output}")


def cmd_all(o):
    me = [sys.executable, str(Path(__file__).resolve())]
    run(["flock", LOCK, *me, "build"])
    outdir = Path(o.outdir)
    for group in ("packet", "classify", "state", "threads"):
        inner = ["flock", LOCK, *me, "measure", "--group", group, "--outdir", outdir,
                 "--rounds", o.rounds]
        if shutil.which("omarchy-benchmark"):
            inner = ["omarchy-benchmark", "--isolate", "--cpu", GROUP_CPUS[group], "--diagnose",
                     "--", *inner]
        subprocess.run([str(c) for c in inner], check=True)
    cmd_summarize(argparse.Namespace(
        inputs=[outdir / f"{g}.json" for g in ("packet", "classify", "state", "threads")],
        output=o.output or outdir / "m0-baseline.json"))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="configure if needed and build both trees (run under flock)")
    b.add_argument("--jobs", type=int, default=8)
    m = sub.add_parser("measure", help="ABBA one group (run under flock and CPU isolation)")
    m.add_argument("--group", required=True, choices=sorted(GROUP_CPUS))
    m.add_argument("--outdir", required=True)
    m.add_argument("--cpus", help="taskset CPU list (default: the group's)")
    m.add_argument("--rounds", type=int, default=6, help="ABBA pairs (even)")
    m.add_argument("--min-time", default="0.05s")
    m.add_argument("--only", action="append", help="restrict to one target (repeatable)")
    m.add_argument("--allow-busy", action="store_true",
                   help="skip the busy-host check outside isolation; marks the result provisional")
    s = sub.add_parser("summarize", help="merge group JSON into one baseline artifact")
    s.add_argument("inputs", nargs="+")
    s.add_argument("--output", required=True)
    a = sub.add_parser("all", help="build, measure every group, summarize")
    a.add_argument("--outdir", required=True)
    a.add_argument("--output")
    a.add_argument("--rounds", type=int, default=6)
    o = p.parse_args()
    if getattr(o, "rounds", 2) < 2 or getattr(o, "rounds", 2) % 2:
        p.error("--rounds must be even and >= 2")
    {"build": cmd_build, "measure": cmd_measure, "summarize": cmd_summarize, "all": cmd_all}[o.cmd](o)


if __name__ == "__main__":
    main()
