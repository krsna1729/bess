#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Nightly benchmark trend and regression attribution.

The nightly job (.github/workflows/nightly-bench.yml) checks out a handful of
develop commits, builds the benchmark subset in one working tree and measures
each commit on the same hosted runner minutes apart.  This tool owns the parts
that are not a build system:

  plan      the commits to measure (N nightly commits, or one commit per day
            for the weekly trend), and the run's metadata;
  targets   the Meson targets of the measured subset (for ci_profile.py);
  run-one   run the subset for one commit into the run file;
  analyze   compare commits inside that one run (never across runs, never
            across hardware) and decide candidate regressions;
  issues    deduplicated GitHub issue actions (open/comment/close);
  history   append one JSONL record of the run for reports.

Decisions only use values measured in the same job: the preceding commits'
samples were taken on the same CPU minutes earlier.  History is a report, not
a baseline; nothing in `analyze` reads it.

Everything runs locally:

    tools/bench_trend.py plan --kind nightly --commits 5 --out run.json --shas
    tools/bench_trend.py run-one --run run.json --commit HEAD --build-dir build/bench
    tools/bench_trend.py analyze --run run.json --out verdicts.json
    tools/bench_trend.py issues --verdicts verdicts.json --history history.jsonl \\
        --open-issues issues.json --repo owner/name --out actions.json

Unit tests: python3 tools/test_bench_trend.py
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The measured subset.  A small, deterministic set that touches independent
# subsystems and avoids threads, sleeps and time-based pacing:
#
#   ring_bench             DPDK ring primitives, caller-owned memory, no EAL
#   fib_bench              LPM/FIB lookup; table setup only (same shape each run)
#   packet_bench           packet alloc/free and header edits on a plain pool
#   packet_checksum_bench  checksum kernels over contiguous packets
#   rcu_bench              read-side RCU pointer reads, ordinary memory
#   utils_copy_bench       memcpy variants on fixed buffer shapes
#   classifier_bench       direct and typed exact-match lookup
#
# Every entry runs a fixed instance count (the filter pins the arguments);
# no benchmark here starts threads or paces work with wall-clock sleeps, the
# two properties that make the rest of the benchmark tree unsuitable for a
# shared hosted runner.  `threshold` is the per-benchmark default (5%).
SUITE = (
    {"binary": "core/ring_bench", "filter": r"^BM_RingRteMpsc/(1|4|16)$"},
    {"binary": "core/fib_bench",
     "filter": r"^BM_Lookup(LpmVec|Fib)/routes:(1024|65536)$"},
    {"binary": "core/packet_bench",
     "filter": r"^BM_(PacketAllocFree|PacketAllocFreeBulk|PacketHeadData|"
               r"PacketAppendTrim|PacketMetadataAccess)(/|$)"},
    {"binary": "core/packet_checksum_bench",
     "filter": r"^BM_RawBessContiguous/packet_bytes:(64|1500)/protocol:6/checksum_intent:0$"
               r"|^BM_ComputeChecksums/packet_bytes:1500/shape:0/protocol:6/"
               r"checksum_intent:0/cold_rotation:0$"},
    {"binary": "core/rcu_bench", "filter": r"^BM_(RawAtomicLoad|RcuPtrRead)(/|$)"},
    {"binary": "core/utils_copy_bench",
     "filter": r"^CopyFixture/(Copy|RteMemcpy)/(0/0/1024|0/0/4096|0/14/1500)$"},
    {"binary": "core/classifier_bench",
     "filter": r"^BM_(DirectExtract|TypedTableLookup)/(1|8|16|32)$"},
)

DEFAULT_THRESHOLD = 0.05
DEFAULT_CV_LIMIT = 0.10
DEFAULT_MIN_TIME = "1s"
DEFAULT_REPETITIONS = 5
DEFAULT_BOOTSTRAP = 10000
DEFAULT_COMMITS = 10
DEFAULT_DAYS = 7
ISSUE_LABEL = "bench-regression"
TITLE_PREFIX = "[bench] "
HISTORY_FILE = "bench-history.jsonl"

_TIME_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


# --------------------------------------------------------------------------
# small statistics helpers
# --------------------------------------------------------------------------

def median(values):
    return statistics.median(values)


def coefficient_of_variation(values):
    """Population CV (stddev / mean); 0 for fewer than two values."""
    if len(values) < 2:
        return 0.0
    mean = statistics.fmean(values)
    if mean == 0:
        return 0.0
    return statistics.pstdev(values) / mean


def percentile(sorted_values, fraction):
    """Linear-interpolated percentile of an already sorted list."""
    if not sorted_values:
        raise ValueError("percentile of an empty list")
    if len(sorted_values) == 1:
        return sorted_values[0]
    position = fraction * (len(sorted_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    weight = position - lower
    return sorted_values[lower] * (1 - weight) + sorted_values[upper] * weight


def bootstrap_ratio_ci(current, baseline, iterations=DEFAULT_BOOTSTRAP, seed=0):
    """95% percentile bootstrap CI for median(current) / median(baseline).

    Resampling both samples is what makes a five-repetition run usable: a CI
    that excludes 1.0 is the evidence that the shift is larger than the
    repetition noise.  The seed is derived from the benchmark and commit so a
    rerun of the analysis gives the same verdict.
    """
    if not current or not baseline:
        return (float("nan"), float("nan"))
    rng = random.Random(seed)
    ratios = []
    for _ in range(iterations):
        c = median([current[rng.randrange(len(current))] for _ in current])
        b = median([baseline[rng.randrange(len(baseline))] for _ in baseline])
        if b > 0:
            ratios.append(c / b)
    if not ratios:
        return (float("nan"), float("nan"))
    ratios.sort()
    return (percentile(ratios, 0.025), percentile(ratios, 0.975))


def _seed(*parts):
    digest = hashlib.sha256("|".join(str(p) for p in parts).encode()).digest()
    return int.from_bytes(digest[:8], "big")


# --------------------------------------------------------------------------
# Google Benchmark JSON
# --------------------------------------------------------------------------

def _time_ns(entry):
    """cpu_time in nanoseconds (Google Benchmark reports the chosen unit)."""
    unit = entry.get("time_unit", "ns")
    return float(entry["cpu_time"]) * _TIME_NS[unit]


def parse_benchmark_json(text):
    """Split one benchmark binary's JSON into per-run-name samples/aggregates.

    `--benchmark_report_aggregates_only=true` emits only aggregate entries
    (median, cv, ...); the sample pass emits one `iteration` entry per
    repetition.  Both are merged per run name so the analysis sees the raw
    timings that the aggregates came from.  A library that writes a warning
    to stdout before the document is tolerated: parsing starts at the first
    object.
    """
    start = text.find("{")
    if start < 0:
        raise ValueError("no JSON object in benchmark output")
    document, _ = json.JSONDecoder().raw_decode(text[start:])
    benchmarks = {}
    for entry in document.get("benchmarks", []):
        name = entry.get("run_name") or entry["name"]
        record = benchmarks.setdefault(name, {"samples": [], "aggregates": {}})
        if entry.get("run_type") == "aggregate":
            record["aggregates"][entry["aggregate_name"]] = _time_ns(entry)
        else:
            record["samples"].append(_time_ns(entry))
    return benchmarks


def benchmark_command(binary, filt, min_time=DEFAULT_MIN_TIME,
                      repetitions=DEFAULT_REPETITIONS, aggregates_only=True):
    """The exact command line of one pass of the subset.

    The aggregate pass is the record's published median/CV; the sample pass
    (same repetition count, without `report_aggregates_only`) is what the
    bootstrap CI is computed from.
    """
    command = [str(binary), f"--benchmark_filter={filt}",
               f"--benchmark_min_time={min_time}",
               f"--benchmark_repetitions={repetitions}"]
    if aggregates_only:
        command.append("--benchmark_report_aggregates_only=true")
    command.append("--benchmark_format=json")
    return command


# --------------------------------------------------------------------------
# run metadata
# --------------------------------------------------------------------------

def hardware_metadata(repo_root, image=None, cpuinfo=None):
    """Model, kernel and runner image: report metadata, never a decision key."""
    cpuinfo_path = Path(cpuinfo) if cpuinfo else Path("/proc/cpuinfo")
    model = ""
    try:
        for line in cpuinfo_path.read_text(errors="replace").splitlines():
            if line.lower().startswith("model name") or line.lower().startswith("hardware"):
                model = line.split(":", 1)[1].strip()
                break
    except OSError:
        model = platform.processor() or platform.machine()
    kernel = platform.release()
    image = image or os.environ.get("ImageVersion") or os.environ.get("ImageOS") or "unknown"
    fingerprint = hashlib.sha256(f"{model}\n{kernel}\n{image}".encode()).hexdigest()[:16]
    return {"model": model or platform.machine(), "kernel": kernel, "runner_image": image,
            "fingerprint": fingerprint, "machine": platform.machine()}


def git(repo_root, *args, check=True):
    return subprocess.run(["git", "-C", str(repo_root), *args], capture_output=True,
                          text=True, check=check).stdout


def commit_metadata(repo_root, sha):
    out = git(repo_root, "show", "-s", "--format=%H%x00%cI%x00%s", sha).strip()
    full_sha, date, subject = out.split("\x00", 2)
    return {"sha": full_sha, "date": date, "subject": subject}


def nightly_commits(repo_root, ref="develop", count=DEFAULT_COMMITS):
    """The last `count` first-parent commits, oldest first.

    First-parent follows the branch a merge joined, so a merge does not put a
    side branch's commits in the timeline.
    """
    out = git(repo_root, "log", "--first-parent", f"-n{count}",
              "--format=%H%x00%cI%x00%s", ref)
    commits = []
    for line in out.splitlines()[:count]:
        if not line.strip():
            continue
        sha, date, subject = line.split("\x00", 2)
        commits.append({"sha": sha, "date": date, "subject": subject})
    return list(reversed(commits))


def weekly_commits(repo_root, ref="develop", days=DEFAULT_DAYS, today=None):
    """One commit per UTC day: the newest commit before each day's 00:00.

    Selecting by timestamp (not by walking parents) gives a calendar-spaced
    trendline even when a day had no push.  Days with no earlier commit are
    skipped, and a commit picked for two boundaries (a quiet day) is kept once.
    """
    if isinstance(today, str):
        today = datetime.fromisoformat(today).replace(tzinfo=timezone.utc)
    now = today or datetime.now(timezone.utc)
    end = now.date()
    selected = []
    seen = set()
    for offset in range(days - 1, -1, -1):
        day = end - timedelta(days=offset)
        boundary = datetime(day.year, day.month, day.day, tzinfo=timezone.utc)
        out = git(repo_root, "rev-list", "-1", f"--before={boundary.isoformat()}",
                  ref, check=False).strip()
        if not out or out in seen:
            continue
        seen.add(out)
        selected.append(commit_metadata(repo_root, out))
    return selected


# --------------------------------------------------------------------------
# plan / run-one
# --------------------------------------------------------------------------

def load_run(path):
    path = Path(path)
    if path.exists():
        return json.loads(path.read_text())
    return {"schema": 1, "commits": []}


def save_run(path, run):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(run, indent=1, sort_keys=True) + "\n")


def plan(args):
    if args.kind == "weekly":
        commits = weekly_commits(args.repo, args.ref, args.days, args.today)
    else:
        commits = nightly_commits(args.repo, args.ref, args.commits)
    run = load_run(args.out) if args.out else {"schema": 1, "commits": []}
    run.update({
        "schema": 1,
        "kind": args.kind,
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "hardware": hardware_metadata(args.repo),
        "commits": commits,
    })
    if args.out:
        save_run(args.out, run)
    if args.shas:
        for commit in commits:
            print(commit["sha"])
    else:
        print(json.dumps({"kind": args.kind, "hardware": run["hardware"],
                          "commits": commits}, indent=1))
    if args.dry_run:
        print(f"# dry-run: would measure {len(commits)} commits, "
              f"suite of {len(SUITE)} binaries", file=sys.stderr)
        for commit in commits:
            print(f"#   {commit['sha'][:12]} {commit['subject']}", file=sys.stderr)
    return 0


def run_one(args):
    build_dir = Path(args.build_dir)
    suite = SUITE
    if args.bench:
        wanted = set(args.bench)
        suite = [entry for entry in SUITE
                 if entry["binary"] in wanted
                 or Path(entry["binary"]).name in wanted
                 or any(w in entry["binary"] for w in wanted)]
        if not suite:
            raise SystemExit(f"--bench {args.bench} matches no suite entry")
    run = load_run(args.run)
    run.setdefault("schema", 1)
    run.setdefault("commits", [])
    run.setdefault("hardware", hardware_metadata(args.repo))
    metadata = commit_metadata(args.repo, args.commit)
    entry = next((c for c in run["commits"] if c["sha"] == metadata["sha"]), None)
    if entry is None:
        entry = metadata
        run["commits"].append(entry)
        run["commits"].sort(key=lambda c: c["date"])
    slot = "confirm" if args.confirm else "sample"
    result = {}
    unavailable = []
    for item in suite:
        binary = build_dir / item["binary"]
        if not args.dry_run and not binary.is_file():
            # An older commit in the run may predate this benchmark: record
            # the rest rather than aborting the timeline.
            print(f'run-one: {item["binary"]} not built; skipping it',
                  file=sys.stderr)
            unavailable.append(item["binary"])
            continue
        record = {"samples": [], "aggregates": {}, "binary": item["binary"],
                  "filter": item["filter"]}
        for aggregates_only, key in ((True, "aggregate"), (False, "samples")):
            command = benchmark_command(binary, item["filter"], args.min_time,
                                        args.repetitions, aggregates_only)
            if args.dry_run:
                print("+ " + " ".join(str(c) for c in command))
                continue
            completed = subprocess.run(command, capture_output=True, text=True)
            if completed.returncode != 0:
                raise SystemExit(f"{command[0]} exited {completed.returncode}:\n"
                                 f"{completed.stderr[-2000:]}")
            parsed = parse_benchmark_json(completed.stdout)
            for name, values in parsed.items():
                merged = result.setdefault(name, {"samples": [], "aggregates": {}})
                if key == "aggregate":
                    merged["aggregates"].update(values["aggregates"])
                    if values["samples"]:
                        merged["samples"].extend(values["samples"])
                else:
                    merged["samples"].extend(values["samples"])
    if args.dry_run:
        return 0
    if not result:
        raise SystemExit(f'no benchmark binary under {build_dir} was found; '
                         'build the subset with "ci_profile.py bench-build"')
    entry[slot] = {"benchmarks": result, "unavailable": unavailable,
                   "when": datetime.now(timezone.utc).isoformat(timespec="seconds")}
    save_run(args.run, run)
    print(f'{entry["sha"][:12]}: {len(result)} instances '
          f'({sum(len(v["samples"]) for v in result.values())} samples, '
          f'{"confirmation" if args.confirm else "first"} run)')
    return 0


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------

def load_config(path):
    config = {"default": DEFAULT_THRESHOLD, "cv_limit": DEFAULT_CV_LIMIT,
              "benchmarks": {}}
    if path:
        loaded = json.loads(Path(path).read_text())
        config.update({k: v for k, v in loaded.items() if k != "benchmarks"})
        config["benchmarks"] = loaded.get("benchmarks", {})
    return config


def threshold_for(name, config):
    overrides = config.get("benchmarks", {})
    if name in overrides:
        return float(overrides[name])
    family = name.split("/", 1)[0]
    if family in overrides:
        return float(overrides[family])
    return float(config.get("default", DEFAULT_THRESHOLD))


def _samples(entry):
    if not entry:
        return []
    return list(entry.get("samples") or [])


def _median_of(entry):
    samples = _samples(entry)
    if samples:
        return median(samples)
    aggregate = (entry or {}).get("aggregates", {}).get("median")
    return float(aggregate) if aggregate is not None else None


def compare_benchmark(name, current, baseline_entries, history_entries,
                      config, iterations, kind):
    """Gate one benchmark at one commit.

    Rules, all measured in this run:
      * ratio of medians above the per-benchmark threshold (default 5%);
      * bootstrap 95% CI of the ratio excludes 1.0;
      * CV of the preceding commits' medians is below 10% (a benchmark that
        has been jittery for the whole run is not evidence of a regression);
      * at least three preceding commits, otherwise stability is unknown.

    For the weekly trend the baseline is the median of the earlier days; for
    the nightly run it is the immediately preceding commit, which was measured
    minutes earlier on the same CPU.
    """
    current_samples = _samples(current)
    history_medians = [m for m in (_median_of(e) for e in history_entries)
                       if m is not None]
    if kind == "weekly":
        baseline_samples = history_medians
        baseline_entry = history_entries[-1] if history_entries else None
    else:
        baseline_entry = baseline_entries[-1] if baseline_entries else None
        baseline_samples = _samples(baseline_entry)
        if not baseline_samples:
            median_value = _median_of(baseline_entry)
            baseline_samples = [median_value] if median_value is not None else []
    result = {
        "benchmark": name,
        "threshold": threshold_for(name, config),
        "ratio": None, "ci": [None, None],
        "current_median": _median_of(current),
        "baseline_median": (median(history_medians) if kind == "weekly" and history_medians
                            else _median_of(baseline_entry)),
        "historical_cv": None, "samples": len(current_samples),
        "eligible": False, "stable": False, "regressed": False, "reasons": [],
    }
    if not current_samples or not baseline_samples or result["current_median"] is None:
        result["reasons"].append("missing samples (run both passes)")
        return result
    ratio = result["current_median"] / result["baseline_median"]
    result["ratio"] = ratio
    low, high = bootstrap_ratio_ci(current_samples, baseline_samples,
                                   iterations=iterations,
                                   seed=_seed(name, result["current_median"]))
    result["ci"] = [low, high]
    if len(history_medians) < 3:
        result["reasons"].append("fewer than three preceding commits")
        return result
    cv = coefficient_of_variation(history_medians)
    result["historical_cv"] = cv
    result["eligible"] = True
    if cv >= float(config.get("cv_limit", DEFAULT_CV_LIMIT)):
        result["reasons"].append(f"historical CV {cv:.1%} is not below "
                                 f"{float(config.get('cv_limit', DEFAULT_CV_LIMIT)):.0%}")
    else:
        # `eligible` says the comparison had enough data; `stable` says the
        # benchmark was calm enough for a clean result to mean something. A
        # high-CV benchmark therefore never contributes recovery evidence.
        result["stable"] = True
    if ratio <= 1.0 + result["threshold"]:
        result["reasons"].append(
            f"ratio {ratio:.3f} is not above threshold {result['threshold']:.0%}")
    if low <= 1.0 <= high:
        result["reasons"].append(f"CI [{low:.3f}, {high:.3f}] includes 1.0")
    result["regressed"] = not result["reasons"]
    return result


def analyze(args):
    run = load_run(args.run)
    commits = run.get("commits", [])
    kind = args.kind or run.get("kind", "nightly")
    if len(commits) < 2:
        # A quiet week can yield one distinct weekly pick. Nothing can be
        # compared inside a one-point run (decisions are made inside one run,
        # never against stored history), but the measurement is still
        # published: zero comparisons, no issues, and the history step records
        # the sample.
        result = {
            "schema": 1,
            "kind": kind,
            "measured_tip": commits[-1]["sha"] if commits else None,
            "hardware": run.get("hardware", {}),
            "commits": [{"sha": c["sha"], "date": c.get("date"),
                         "subject": c.get("subject")} for c in commits],
            "comparisons": [],
            "flagged": [],
            "needs_rerun": [],
        }
        Path(args.out).write_text(json.dumps(result, indent=1) + "\n")
        print("fewer than two commits: no comparison; the measurement is "
              "still published")
        return 0
    config = load_config(args.config)
    entries = []
    needs_rerun = {}
    samples_by_commit = []
    for index, commit in enumerate(commits):
        samples_by_commit.append((commit.get("sample") or {}).get("benchmarks", {}))
    for index in range(1, len(commits)):
        current_benchmarks = samples_by_commit[index]
        history = [samples_by_commit[i] for i in range(index)]
        current_confirm = (commits[index].get("confirm") or {}).get("benchmarks", {})
        for name in sorted(current_benchmarks):
            baseline_entries = [h[name] for h in history if name in h]
            comparison = compare_benchmark(name, current_benchmarks[name],
                                           baseline_entries, baseline_entries,
                                           config, args.bootstrap_iterations, kind)
            comparison["commit"] = commits[index]["sha"]
            comparison["baseline_commit"] = commits[index - 1]["sha"]
            comparison["confirmed"] = False
            comparison["confirmation_failed"] = False
            if comparison["regressed"] and name in current_confirm:
                confirm = compare_benchmark(
                    name, current_confirm[name], baseline_entries, baseline_entries,
                    config, args.bootstrap_iterations, kind)
                confirmed = (confirm["ratio"] is not None
                             and confirm["ratio"] > 1.0 + comparison["threshold"]
                             and confirm["ci"][0] > 1.0)
                comparison["confirmed"] = confirmed
                comparison["confirm_ratio"] = confirm["ratio"]
                comparison["confirm_ci"] = confirm["ci"]
                if not confirmed:
                    comparison["confirmation_failed"] = True
                    comparison["reasons"].append("confirmation run did not reproduce")
            if comparison["regressed"] and not comparison["confirmed"] \
                    and not comparison["confirmation_failed"]:
                needs_rerun.setdefault(commits[index]["sha"], []).append(name)
            entries.append(comparison)
    # Attribution: the first commit in this run that already crossed the gate,
    # ignoring candidates a confirmation rejected and candidates the run later
    # recovered from (an eligible clean measurement before the flagged commit).
    by_benchmark = {}
    for entry in entries:
        by_benchmark.setdefault(entry["benchmark"], []).append(entry)
    for entry in entries:
        if not entry["confirmed"]:
            continue
        sequence = by_benchmark[entry["benchmark"]]
        position = sequence.index(entry)
        commit_range = {"first_bad": entry["commit"],
                        "last_good": entry["baseline_commit"]}
        for offset, candidate in enumerate(sequence[:position]):
            if not candidate["regressed"]:
                continue
            if candidate.get("confirmation_failed"):
                continue
            recovered = any(later.get("eligible") and later.get("stable")
                            and not later["regressed"]
                            for later in sequence[offset + 1:position + 1])
            if recovered:
                continue
            commit_range = {"first_bad": candidate["commit"],
                            "last_good": candidate["baseline_commit"]}
            break
        entry["commit_range"] = commit_range
    flagged = [entry for entry in entries if entry["confirmed"]]
    measured = [commit["sha"] for commit, sample in zip(commits, samples_by_commit)
                if sample]
    measured_tip = measured[-1] if measured else commits[-1]["sha"]
    result = {
        "schema": 1,
        "kind": kind,
        "measured_tip": measured_tip,
        "hardware": run.get("hardware", {}),
        "commits": [{"sha": c["sha"], "date": c.get("date"),
                     "subject": c.get("subject")} for c in commits],
        "comparisons": entries,
        "flagged": flagged,
        "needs_rerun": [{"commit": sha, "benchmarks": names}
                        for sha, names in needs_rerun.items()],
    }
    Path(args.out).write_text(json.dumps(result, indent=1) + "\n")
    print(f"{len(entries)} comparisons, {len(flagged)} confirmed regression(s), "
          f"{len(needs_rerun)} commit(s) need a confirmation run")
    for entry in flagged:
        print(f'  {entry["benchmark"]}: {entry["ratio"]:.3f} '
              f'CI [{entry["ci"][0]:.3f}, {entry["ci"][1]:.3f}] '
              f'at {entry["commit"][:12]}')
    return 0


# --------------------------------------------------------------------------
# issues
# --------------------------------------------------------------------------

def _issue_benchmark(issue):
    title = issue.get("title", "")
    if not title.startswith(TITLE_PREFIX):
        return None
    return title[len(TITLE_PREFIX):].split(" ", 1)[0]


def _issue_labels(issue):
    return {label.get("name") if isinstance(label, dict) else label
            for label in issue.get("labels", [])}


def _open_issues_for(open_issues, benchmark):
    return sorted(
        (issue for issue in open_issues
         if _issue_benchmark(issue) == benchmark
         and (not _issue_labels(issue) or ISSUE_LABEL in _issue_labels(issue))),
        key=lambda issue: issue["number"])


def _marker(benchmark, commit):
    return f"<!-- bench-trend:{benchmark}:{commit} -->"


def _issue_has_marker(issue, marker):
    """A marker may be in the issue body or in any comment already posted."""
    if marker in (issue.get("body") or ""):
        return True
    for comment in issue.get("comments") or []:
        body = comment.get("body") if isinstance(comment, dict) else comment
        if marker in (body or ""):
            return True
    return False


def issue_body(entry, repo, history_url, run_url):
    ratio = entry["ratio"]
    low, high = entry["ci"]
    cv = entry.get("historical_cv") or 0.0
    commit_range = entry.get("commit_range", {})
    first_bad = commit_range.get("first_bad", entry["commit"])
    last_good = commit_range.get("last_good", entry["baseline_commit"])
    lines = [
        f'{entry["benchmark"]} is {(ratio - 1):.1%} slower than its baseline '
        "in this run.",
        "",
        f'- commit range: `{last_good[:12]}`..`{first_bad[:12]}`',
        f'- benchmarked commit: `{entry["commit"]}`',
        f'- baseline median: {entry["baseline_median"]:.3f} ns, '
        f'current median: {entry["current_median"]:.3f} ns',
        f'- ratio: {ratio:.4f} (threshold {entry["threshold"]:.0%})',
        f'- bootstrap 95% CI: [{low:.4f}, {high:.4f}] (excludes 1.0)',
        f'- historical CV in this run: {cv:.1%}',
        f'- hardware: {entry.get("hardware_model", "see run record")}',
        f'- confirmation: `{entry.get("confirm_ratio", 0):.4f}` '
        f'CI [{entry.get("confirm_ci", [0, 0])[0]:.4f}, '
        f'{entry.get("confirm_ci", [0, 0])[1]:.4f}]',
        "",
        f'Run: {run_url}',
        f'History (JSONL on the `benchmarks` branch): {history_url}',
        "",
        "Decisions compare commits measured in the same job on the same runner; "
        "previous nights are not used as a baseline.",
        "",
        _marker(entry["benchmark"], entry["commit"]),
    ]
    return "\n".join(lines)


def plan_issue_actions(verdicts, history, open_issues, repo, history_url, run_url):
    """One open issue per benchmark, updated in place; duplicates are closed.

    Deduplication is by issue title (`[bench] <name> regression`) among open
    issues; a comment for the same benchmark and commit is skipped when the
    issue body or any existing comment already carries that marker.  An issue
    closes after the benchmark has been clean for two consecutive runs (the
    current run plus the immediately preceding history record), and only an
    eligible, stable and non-null verdict at the run's final measured commit
    advances recovery; a record that never measured the benchmark does not
    count as a clean night.
    """
    flagged = {entry["benchmark"]: entry for entry in verdicts.get("flagged", [])}
    latest = {}
    for entry in verdicts.get("comparisons", []):
        latest[entry["benchmark"]] = entry
    measured_tip = verdicts.get("measured_tip") or (
        verdicts.get("commits") or [{}])[-1].get("sha")
    records = list(history)
    actions = []
    for benchmark, entry in sorted(flagged.items()):
        entry = dict(entry)
        entry["hardware_model"] = verdicts.get("hardware", {}).get("model", "unknown")
        body = issue_body(entry, repo, history_url, run_url)
        issues = _open_issues_for(open_issues, benchmark)
        if not issues:
            actions.append({"action": "open", "benchmark": benchmark,
                            "title": f"{TITLE_PREFIX}{benchmark} regression",
                            "body": body, "labels": [ISSUE_LABEL]})
        else:
            primary, duplicates = issues[0], issues[1:]
            marker = _marker(benchmark, entry["commit"])
            if not _issue_has_marker(primary, marker):
                actions.append({"action": "comment", "benchmark": benchmark,
                                "number": primary["number"], "body": body})
            for duplicate in duplicates:
                actions.append({"action": "close", "benchmark": benchmark,
                                "number": duplicate["number"],
                                "body": f"Duplicate of #{primary['number']}; "
                                        "one open issue per benchmark."})
    for benchmark in sorted(set(latest) - set(flagged)):
        issues = _open_issues_for(open_issues, benchmark)
        if not issues:
            continue
        entry = latest[benchmark]
        # Recovery needs a stable verdict at the run's final measured commit:
        # samples present, enough preceding commits, the CV gate passed, and
        # no regression. A benchmark absent from the tip (or measured only
        # earlier in the run) has no clean evidence.
        if entry.get("commit") != measured_tip:
            continue
        if not (entry.get("eligible") and entry.get("stable")
                and entry.get("ratio") is not None
                and not entry.get("regressed")):
            continue
        primary, duplicates = issues[0], issues[1:]
        clean = 1  # the current run
        previous_flagged = False
        for record in reversed(records):
            if benchmark in record.get("flagged", []):
                previous_flagged = True
                break
            if benchmark in record.get("clean", []):
                clean += 1
            else:
                # The record did not produce an eligible clean verdict for
                # this benchmark: it neither advances nor resets recovery.
                break
        if clean >= 2:
            body = (f"No regression in the last {clean} benchmark runs; closing. "
                    "A future run opens a new issue.")
            actions.append({"action": "close", "benchmark": benchmark,
                            "number": primary["number"], "body": body})
        elif previous_flagged:
            body = ("No regression in this run. The issue closes after the next "
                    "clean run.")
            actions.append({"action": "comment", "benchmark": benchmark,
                            "number": primary["number"], "body": body})
        for duplicate in duplicates:
            actions.append({"action": "close", "benchmark": benchmark,
                            "number": duplicate["number"],
                            "body": f"Duplicate of #{primary['number']}."})
    return actions


def apply_actions(actions, repo, dry_run=False):
    for action in actions:
        if action["action"] == "open":
            command = ["gh", "issue", "create", "--repo", repo,
                       "--title", action["title"], "--body", action["body"]]
            for label in action.get("labels", []):
                command += ["--label", label]
        elif action["action"] == "comment":
            command = ["gh", "issue", "comment", str(action["number"]),
                       "--repo", repo, "--body", action["body"]]
        elif action["action"] == "close":
            command = ["gh", "issue", "close", str(action["number"]),
                       "--repo", repo, "--comment", action["body"]]
        else:
            raise SystemExit(f'unknown action {action["action"]}')
        if dry_run:
            print("+ " + " ".join(command[:4]))
            continue
        subprocess.run(command, check=True)
    return 0


def issues(args):
    verdicts = json.loads(Path(args.verdicts).read_text())
    history = []
    if args.history and Path(args.history).exists():
        for line in Path(args.history).read_text().splitlines():
            if line.strip():
                history.append(json.loads(line))
    open_issues = []
    if args.open_issues and Path(args.open_issues).exists():
        text = Path(args.open_issues).read_text()
        open_issues = json.loads(text) if text.strip() else []
    history_url = args.history_url or (
        f"https://github.com/{args.github_repo}/blob/benchmarks/{HISTORY_FILE}")
    actions = plan_issue_actions(verdicts, history, open_issues, args.github_repo,
                                 history_url, args.run_url or "")
    Path(args.out).write_text(json.dumps(actions, indent=1) + "\n")
    for action in actions:
        print(f'{action["action"]:8} {action["benchmark"]}'
              + (f' #{action["number"]}' if action.get("number") else ""))
    if args.apply:
        apply_actions(actions, args.github_repo, dry_run=args.dry_run)
    return 0


# --------------------------------------------------------------------------
# history
# --------------------------------------------------------------------------

def history(args):
    run = load_run(args.run)
    verdicts = json.loads(Path(args.verdicts).read_text())
    comparisons = verdicts.get("comparisons", [])
    per_benchmark = {}
    latest = {}
    for entry in comparisons:
        per_benchmark.setdefault(entry["benchmark"], []).append({
            "commit": entry["commit"],
            "ratio": entry["ratio"],
            "median_ns": entry["current_median"],
            "baseline_ns": entry["baseline_median"],
            "ci": entry["ci"],
            "cv": entry["historical_cv"],
            "eligible": entry.get("eligible", False),
            "stable": entry.get("stable", False),
            "regressed": entry.get("regressed", False),
        })
        latest[entry["benchmark"]] = entry
    measured_tip = verdicts.get("measured_tip") or (
        verdicts.get("commits") or [{}])[-1].get("sha")
    # Recovery evidence: a stable, eligible clean verdict at the final measured
    # commit. A benchmark that was last measured earlier in the run is not
    # evidence that the tip is clean.
    clean = sorted(
        name for name, entry in latest.items()
        if entry.get("commit") == measured_tip
        and entry.get("eligible") and entry.get("stable")
        and entry.get("ratio") is not None
        and not entry.get("regressed"))
    record = {
        "schema": 1,
        "date": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "kind": verdicts.get("kind", run.get("kind")),
        "commit": measured_tip,
        "hardware": run.get("hardware", {}),
        "commits": run.get("commits", []),
        "benchmarks": per_benchmark,
        "flagged": [entry["benchmark"] for entry in verdicts.get("flagged", [])],
        "clean": clean,
        "run_url": args.run_url or "",
    }
    path = Path(args.history)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")
    print(f"appended one record ({len(record['benchmarks'])} benchmarks, "
          f"{len(record['commits'])} commits) to {path}")
    return 0


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def targets(args):
    print(" ".join(entry["binary"] for entry in SUITE))
    return 0


def rerun_shas(args):
    """Commits that need the confirmation sample, in run order (bounded)."""
    verdicts = json.loads(Path(args.verdicts).read_text())
    seen = []
    for item in verdicts.get("needs_rerun", []):
        if item["commit"] not in seen:
            seen.append(item["commit"])
    for sha in seen[:args.limit]:
        print(sha)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=str(ROOT),
                        help="working tree (git root; default: this checkout)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("plan", help="resolve the commits of one run")
    p.add_argument("--kind", choices=["nightly", "weekly"], default="nightly")
    p.add_argument("--ref", default="develop")
    p.add_argument("--commits", type=int, default=DEFAULT_COMMITS)
    p.add_argument("--days", type=int, default=DEFAULT_DAYS)
    p.add_argument("--today", help="ISO date, for a reproducible weekly plan")
    p.add_argument("--out", help="write the run file skeleton here")
    p.add_argument("--shas", action="store_true", help="print only the SHAs")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=plan)

    p = sub.add_parser("run-one", help="measure one commit into the run file")
    p.add_argument("--run", required=True)
    p.add_argument("--commit", required=True)
    p.add_argument("--build-dir", default="build/bench")
    p.add_argument("--min-time", default=DEFAULT_MIN_TIME)
    p.add_argument("--repetitions", type=int, default=DEFAULT_REPETITIONS)
    p.add_argument("--bench", nargs="+", help="suite binaries (default: all)")
    p.add_argument("--confirm", action="store_true",
                   help="store the second sample for confirmation")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=run_one)

    p = sub.add_parser("analyze", help="compare commits inside one run")
    p.add_argument("--run", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--kind", choices=["nightly", "weekly"])
    p.add_argument("--config", help="per-benchmark thresholds (JSON)")
    p.add_argument("--bootstrap-iterations", type=int, default=DEFAULT_BOOTSTRAP)
    p.set_defaults(func=analyze)

    p = sub.add_parser("issues", help="plan or apply deduplicated issue actions")
    p.add_argument("--verdicts", required=True)
    p.add_argument("--history", help="JSONL of previous runs (recovery only)")
    p.add_argument("--open-issues", help="gh issue list --json number,title,body,labels")
    p.add_argument("--out", required=True)
    p.add_argument("--github-repo", required=True,
                   help="owner/name (the working tree is the global --repo)")
    p.add_argument("--history-url")
    p.add_argument("--run-url")
    p.add_argument("--apply", action="store_true", help="run the gh commands")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=issues)

    p = sub.add_parser("history", help="append the run's JSONL record")
    p.add_argument("--run", required=True)
    p.add_argument("--verdicts", required=True)
    p.add_argument("--history", required=True)
    p.add_argument("--run-url")
    p.set_defaults(func=history)

    p = sub.add_parser("targets", help="print the Meson targets of the subset")
    p.set_defaults(func=targets)

    p = sub.add_parser("rerun-shas",
                       help="print commits that need a confirmation run")
    p.add_argument("--verdicts", required=True)
    p.add_argument("--limit", type=int, default=3,
                   help="rerun at most this many commits (default 3); the "
                        "nightly path stays bounded when noise flags several")
    p.set_defaults(func=rerun_shas)

    args = parser.parse_args(argv)
    args.repo = Path(args.repo)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
