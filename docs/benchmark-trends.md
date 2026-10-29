# Benchmark trends

The benchmark workflow measures a small, stable subset of the benchmark tree
at several `develop` commits **in one job on one runner**, and flags a commit
only when it is slower than the commits measured minutes earlier in that same
run. Nothing is compared across nights or across runners, so a change of
hosted-runner CPU cannot create or hide a regression. The `benchmarks` branch
keeps a JSONL report of each run; it is history, not a baseline.

Workflow: `.github/workflows/nightly-bench.yml`.
Tools: `tools/bench_trend.py`, `tools/bench_thresholds.json`.

## Schedule and shape

| Run | When | Commits |
|---|---|---|
| nightly | daily, 04:47 UTC | the last 10 first-parent commits of `develop` |
| weekly | Sundays, 05:47 UTC | one commit per UTC day for the last 7 days: the newest commit before each day's 00:00 UTC boundary |

Both run on one `ubuntu-24.04` x86_64 runner with the gating gcc lane's pinned
compiler (`--cc gcc-14 --cxx g++-14 --release-semantics`: `-O3`, NDEBUG, the
same `-Dcpu=x86-64-v3` floor as every other lane). `workflow_dispatch` exposes
`kind`, `commits` (nightly, default 10), `days` (weekly, default 7) and
`dry_run`; `dry_run` prints the plan and every command without building,
opening issues or pushing history.

The weekly plan skips a UTC day that had no commit before its midnight, so a
quiet week contributes fewer points; the rules need three preceding
measurements, so a run with fewer than four commits (or fewer than four
distinct weekly picks) reports the values but cannot flag a regression.

Nightly attribution is `--commits N`: the job checks out each of the last N
commits in the same working tree, rebuilds only the benchmark targets
(`tools/ci_profile.py bench-build`, ccache across commits), and measures it.
The weekly run is the same machinery over calendar days, with the latest day
compared against the median of the earlier days.

## The measured subset

`SUITE` in `tools/bench_trend.py` is the single definition; `bench_trend.py
targets` prints the Meson targets. The subset is small on purpose and chosen
for determinism on a shared hosted runner: single-threaded, fixed-size work,
no wall-clock pacing, no dependency on an idle machine.

| Binary | Instances | Why |
|---|---|---|
| `ring_bench` | `BM_RingRteMpsc/1,2` | DPDK ring primitives in caller-owned memory; no EAL. Producer counts above the allowed CPUs minus one skip themselves, and a hosted runner has four CPUs, so 4 and 16 are not in the subset |
| `fib_bench` | `BM_LookupLpmVec`, `BM_LookupFib` at 1K and 64K routes | routing lookup, the most latency-sensitive dataplane path; table setup is identical per run |
| `packet_bench` | alloc/free, bulk alloc/free, head data, append/trim, metadata | packet primitives every pipeline touches; plain pool, no hugepages |
| `packet_checksum_bench` | one contiguous raw-checksum shape and one computed-checksum shape | checksum kernels, fixed packet bytes |
| `rcu_bench` | `BM_RawAtomicLoad`, `BM_RcuPtrRead` | read-side RCU; ordinary memory, no EAL |
| `utils_copy_bench` | `CopyFixture/Copy` and `RteMemcpy` at three fixed shapes | copy paths used on the packet hot path |
| `classifier_bench` | `BM_DirectExtract`, `BM_TypedTableLookup` at batches 1, 8, 16, 32 | classification/extraction, the first pipeline stage |

Excluded deliberately: anything that starts threads or paces work with sleeps
(`stats_bench`, `transaction_bench`, `mempool_bench` pipeline families, the
RCU grace-period family), and any benchmark whose argument product would run
hundreds of instances (for example `BM_ComputeChecksums`' full product).

Every binary runs twice per commit:

```
<binary> --benchmark_filter=<instance regex> --benchmark_min_time=1s \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
    --benchmark_format=json
<binary> --benchmark_filter=<instance regex> --benchmark_min_time=1s \
    --benchmark_repetitions=5 --benchmark_format=json
```

The first pass is the published median and CV (Google Benchmark's aggregates).
The second is the same five repetitions as raw samples, because the
`--benchmark_report_aggregates_only` format does not contain them and the
bootstrap confidence interval needs them.

**Expected runtime.** About 28–31 instances × 5 repetitions × 1 s ≈ 150 s per
pass, ≈ 5 minutes of measurement per commit, plus an incremental build of the
subset. Ten commits are expected to take roughly 60–90 minutes on a hosted
runner (first commit near a full benchmark-target build, the rest ccache and
incremental); `--commits 5` roughly halves it. The job times out after 240
minutes.

## Decision rules

`tools/bench_trend.py analyze` compares, for every benchmark at every commit
after the first, the current commit's median with the preceding commit's
(nightly) or with the median of the earlier days (weekly). A regression is a
candidate only when **all** of these hold, evaluated inside the run:

1. **Threshold.** `current median / baseline median` is above
   `1 + threshold`. The default is 5%; `tools/bench_thresholds.json` overrides
   it globally (`default`) or per benchmark (`benchmarks`, keyed by run name
   or family name — e.g. `"BM_RcuPtrRead": 0.08`). `cv_limit` (default 0.10)
   is configurable there too.
2. **Bootstrap CI.** The 95% percentile bootstrap CI of the ratio, resampling
   both samples (10 000 iterations, deterministic seed per benchmark and
   value), excludes 1.0.
3. **Historical CV.** The coefficient of variation of the preceding commits'
   medians is below 10%. A benchmark that has been jittery for the whole run
   is not evidence of a regression; the comparison records `stable: false`, and
   such a verdict never counts as clean for recovery. Fewer than three
   preceding commits means stability is unknown; those commits never flag.
4. **Confirmation.** The candidate commit is checked out again, rebuilt
   incrementally and measured a second time in the same job. The issue opens
   only if the second sample also exceeds the threshold with a CI excluding
   1.0. `needs_rerun` in the verdicts names the commit; the workflow reruns at
   most three candidates (`rerun-shas --limit`) to bound the job.

When a later commit flags a benchmark first crossed at an earlier commit, the
issue reports that commit range (`first_bad`..`last_good`); attribution is a
scan of this run's measurements, not a bisect.

## Issues

`bench_trend.py issues` plans and (with `--apply`) performs:

- **One open issue per benchmark**, title `[bench] <name> regression`, label
  `bench-regression` (created by the workflow when absent). If an open issue
  already matches that title, the tool comments instead of opening another.
  Duplicate open issues are closed against the oldest. A comment is skipped
  when the issue body already
  carries this run's marker (`<!-- bench-trend:<benchmark>:<commit> -->`), so
  re-running the same run is idempotent.
- **Recovery.** When the benchmark does not flag in a run and did flag in the
  previous history record, the first clean run comments; a second consecutive
  clean run closes the issue. A run is clean only when the benchmark has an
  eligible (enough preceding commits), stable (CV gate passed), completed
  verdict at that run's final measured commit; a run that never measured it,
  or measured it only earlier in the window, neither advances nor resets
  recovery. "Consecutive runs" means consecutive workflow records (nightly
  and weekly count alike).

The issue body carries the commit range, measured commit, baseline/current
medians, ratio, threshold, CI, CV, confirmation ratio/CI, hardware model, the
workflow run URL and the history-branch link.

Required permissions are declared in the workflow: `contents: write` to push
the history branch and `issues: write` to open/comment/close issues, through
`GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}`.

## History branch

The workflow fetches `benchmarks` (creating an orphan branch on first use),
appends one line to `bench-history.jsonl`, and pushes as
`github-actions[bot]`. A concurrency group serialises workflow runs; the push
retries with a rebase if another writer landed first. The file is append-only.

One record per run:

```json
{"schema":1,"date":"...","kind":"nightly","commit":"<tip sha>",
 "hardware":{"model":"...","kernel":"...","runner_image":"...","fingerprint":"..."},
 "commits":[{"sha":"...","date":"...","subject":"..."}],
 "benchmarks":{"BM_RingRteMpsc/2":[{"commit":"...","ratio":1.012,
   "median_ns":48.3,"baseline_ns":47.7,"ci":[0.99,1.04],"cv":0.008,
   "eligible":true,"stable":true,"regressed":false}]},
 "flagged":["..."],"clean":["..."],"run_url":"..."}
```

`flagged` lists the benchmarks this run confirmed; `clean` lists benchmarks
whose verdict at the run's final measured commit was eligible (samples, at
least three preceding commits), stable (CV gate) and not a regression.
Recovery counts only consecutive records that list the benchmark in `clean`; a
record that never measured it does not advance the count.

Hardware is report metadata; the analysis never reads this file. The workflow
also uploads `run.json` and `verdicts.json` as an artifact of every run.

## Running it locally

The tool has no third-party dependencies.

```sh
# unit tests (no GitHub, no compiler, no network)
python3 tools/test_bench_trend.py
python3 -m unittest discover -s tools -p 'test_*.py'

# what the nightly job would measure, without building or publishing
python3 tools/bench_trend.py plan --kind nightly --commits 5 --shas
python3 tools/bench_trend.py plan --kind weekly --days 7

# measure the current checkout (after ci_profile.py bench-build) into a run
python3 tools/ci_profile.py bench-build --compiler gcc --name bench \
    --release-semantics --bench-targets $(python3 tools/bench_trend.py targets)
python3 tools/bench_trend.py run-one --run /tmp/run.json --commit HEAD \
    --build-dir build/bench

# re-run the analysis against any run file
python3 tools/bench_trend.py analyze --run /tmp/run.json --out /tmp/verdicts.json \
    --config tools/bench_thresholds.json
```

`run-one --dry-run` prints the exact binaries and flags. The analysis only
needs a run file, so a saved artifact can be re-analysed offline. To exercise
the issue planner locally, feed it the verdicts plus
`gh issue list --json number,title,body,labels > /tmp/issues.json`; without
`--apply` it only writes `actions.json`.

## Residual risks

- **Hosted-runner variance.** A single runner's frequency, thermal and
  neighbour state still moves individual numbers. Within-run comparison,
  five repetitions, the bootstrap CI, the CV gate and a second sample are the
  mitigations; the cost is that a regression smaller than the 5% threshold
  (or one that appears only on another CPU model) is not reported.
- **Drift across the run.** Commits are measured oldest first, so a runner
  that heats up over an hour can look like a slow trend. The confirmation
  sample is taken shortly after the candidate, but a sustained thermal drift
  can still survive it; the weekly run is the cross-check.
- **Builds, not just measurements.** Each commit is rebuilt; a compiler or
  DPDK change inside the N commits can dominate the delta. DPDK is
  bootstrapped once at the run's tip, so a `deps/dpdk.json` change inside the
  window is a known mismatch.
- **Renamed benchmarks.** The subset is a regex list. A rename silently drops
  an instance from the run; the fewer instances are visible in the run record
  and artifact.
- **Token permissions.** `contents: write` and `issues: write` must be
  permitted for the repository's `GITHUB_TOKEN` (the default restricts it in
  some organisations). Without them the history push or the issue step fails
  loudly.
- **Schedules.** GitHub disables scheduled workflows after 60 days without
  repository activity; `workflow_dispatch` is the manual fallback.
