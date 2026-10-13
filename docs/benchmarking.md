# Benchmarking BESS

How to produce numbers that can be trusted and compared. Decisions in
[decisions.md](decisions.md) cite benchmarks run this way (D-016).

## Build

- Benchmark a **release** build compiled for the machine you measure on:
  `meson setup build/bench -Dcpu=native --buildtype=release`.
  `-Dcpu=x86-64-v3` also works when the result must represent a portable
  build.
- Do not quote numbers from a portable-baseline test build. Older baselines
  such as `corei7` lack BMI1/AVX2: the compiler emits `bsf` for bit scans,
  which is 6 uops on AMD Zen 3 against `tzcnt`'s 2.
- DPDK should be built for the same target (`tools/bootstrap_dpdk.py
  --cpu`).
- `-Dtune=<name>` changes only `-mtune` (the scheduling model). Compare tunes
  of one compiler, from one tree, in one paired run; the matrix below is how.
  Compilers do not share a default: at `-Dcpu=x86-64-v3` GCC tunes `generic`
  and Clang `haswell`.

## Machine state

- **Pin each benchmark to one CPU, and isolate that CPU from other
  tasks** where you can: a cgroup v2 isolated partition, `isolcpus`, or at
  least `taskset`. Use the performance governor.
- **On hybrid CPUs, measure on each core type** (for example one P-core
  and one E-core); results differ materially.
- **Make sure the machine is otherwise idle.** A stray busy process never
  shares the isolated CPU, but it shares the chip's power, thermal and
  cache budget, which moves absolute numbers. Check with
  `ps -eo pid,etime,pcpu,args --sort=-pcpu | head`. `tools/ab_bench.py`
  refuses to start while another process uses more than 20% of a CPU.
- Run benchmarks under `timeout`, so a hang cannot keep spinning after
  the session.
- Multi-threaded benchmarks must pin each thread themselves: the DPDK EAL
  pins only the main thread, and an isolated partition does not load
  balance.
- Tables meant to exceed the caches need key streams far larger than the
  caches; a small cycling stream stays cache-resident.

## Comparing A and B

Use `tools/ab_bench.py`, not two separate runs:

```
tools/ab_bench.py OLD_BINARY NEW_BINARY --filter 'BM_Lookup' \
    --rounds 8 --wrap "taskset -c 2"
```

- **ABBA order** (A B B A A B B A …): slow drift such as thermal state,
  frequency or background load affects both sides equally.
- **Paired ratios:** each A run is compared with the B run next to it.
  The report gives the median ratio, its min..max, and how many pairs
  agree.
- **The significance rule:** a difference is called only when the median
  ratio is outside ±3% and at least 3/4 of pairs agree on its sign.
- **Two implementations in one binary:** use `--filter-b` with
  `--rename-b 'PATTERN=>REPLACEMENT'` to pair their names.
- **Reporting:** state the machine (CPU model and core type), the build
  (`-march`, buildtype, compiler, DPDK version), and the command.

## Live packet rate under transactions

`tools/live_transaction_bench.py` measures a running bessd: packets
classified by a real ExactMatch module on one worker while a client changes
its rules through the transaction RPC (D-025) at given rates (each
transaction adds one session's rule and removes the oldest). It prints Mpps
through the module, the hit fraction (about 0.5 by construction, which
checks that rules steer packets), transactions/s achieved and client-side
latency. Start bessd isolated on the worker core as for any benchmark, run
the script from another core with `PYTHONPATH` set to the generated Python
protobufs (`build/protobuf/generated/python` and its `builtin_pb`), and
compare rates within one run (`--rounds` interleaves them).

## Flow-state tables

`flow_bench` (`core/flow/flow_bench.cc`, D-052) covers `WorkerFlowTable` and
`SharedFlowTable` against the backends they were chosen over. Build and run it
from the release tree, pinned to one P-core for the single-thread benchmarks:

```
ninja -C build/perf-release core/flow_bench
taskset -c 2 build/perf-release/core/flow_bench --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    --benchmark_filter='BM_Worker|BM_Baseline'
```

`BM_SharedLookup` is the one-thread row of `SharedFlowTable` (scalar and batched
`Peek`, hit and miss, 64K and 1M flows): its `real_time` is one lookup (or one
batch), so `tools/ab_bench.py` can pair it A/B when the shared read path changes.
The multi-thread rows below report `Mlookups_s` as a counter and a nominal
`real_time` (a 5 ms sleep per iteration), so `ab_bench.py` cannot compare them;
pair those by hand (alternate the two binaries ABBA, compare the counter).

The multi-thread benchmarks (`BM_SharedReaders`, `BM_SharedReaderWriter`) pin
their own threads to the CPUs the process may use, except the CPU the
benchmark thread sleeps on. Give the process one CPU more than the threads it
should start, and set `FLOW_BENCH_MAIN_CPU` to that spare CPU:

```
FLOW_BENCH_MAIN_CPU=0 taskset -c 0,2,4,6,8,10 flow_bench \
    --benchmark_filter='BM_SharedReaders/[0-9]+/(1|2|4)/[01]/'
```

`FLOW_BENCH_LARGE=1` also registers the 10M-flow cases: the worker table with the
smallest key and State (about 0.5 GB) and `BM_SharedLookup` at 10M (about 1.8 GB
of EAL heap). It also brings the EAL up on the 4 KB-page no-hugepage heap (3,000
MB, or `BESS_DPDK_NOHUGE_MB`) instead of hugepages, for every EAL-backed row in
that process (`BM_Shared*`, `BM_BaselineRteHash`, `BM_BaselineConcurrentExact`), so
compare a LARGE run only with another LARGE run. Lookup benchmarks report
`ns_per_lookup`, `tsc_per_lookup` (TSC ticks, not core cycles) and `bytes_per_flow`.

## Expiry

`expiry_bench` (`core/dataplane/expiry_bench.cc`, D-053) runs the same
schedule / refresh / cancel / poll operations through the mechanisms the roadmap
says to evaluate (the wheel, an eager-refresh wheel, a hashed single-level wheel, a
heap with lazy deletion, a periodic scan, DPDK `rte_timer`); `flow_expiry_bench`
(`core/flow/flow_expiry_bench.cc`) measures what each way of refreshing a flow
costs a packet loop over a `WorkerFlowTable`, and the cost of a scan of the table.
Run them from the release tree on one P-core, one candidate at a time (the EAL
comes up in the first benchmark, so `LD_LIBRARY_PATH` must name DPDK's libraries):

```
ninja -C build/perf-release core/expiry_bench core/flow_expiry_bench
taskset -c 2 build/perf-release/core/expiry_bench --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true --benchmark_filter='^BM_[A-Za-z]+/wheel/'
taskset -c 2 build/perf-release/core/flow_expiry_bench --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true
```

`EXPIRY_BENCH_LARGE=1` also registers 10M timers for the wheel and the scan.
Timing is in ticks of one nanosecond; the drain benchmarks report
`ns_per_expiry`, `worst_poll_ns` and `p999_poll_ns` (single poll, TSC-timed),
and every benchmark reports `bytes_per_timer`. `rte_timer` reads the real TSC and
has no budget, so its expiring distributions are compressed to a few microseconds
(the comment in the source says how). The hashed wheel is a baseline only (see
its comment) and is not run at 1M timers over an hour: that is 440M visits.

## Handoff

`handoff_bench` (`core/dataplane/handoff_bench.cc`, D-054) runs the same
handoff through the candidates the roadmap says to compare: the Queue module's
exact ring calls (a ring of bare pointers, single-producer enqueue, single-consumer
dequeue), `rte_ring_elem` rings with 8 to 64 byte elements, the flag-dispatching
generic calls, multi-producer variants, a cached-index single-producer ring
(a yardstick only; no such queue ships), and the real `HandoffChannel`. It needs
two CPUs and pins its own two threads to the first two CPUs the process may use,
so run it under `taskset` with two **physical** cores (on this machine CPUs 2 and
4: siblings share a core), one candidate family at a time:

```
ninja -C build/perf-release core/handoff_bench
export LD_LIBRARY_PATH=$PWD/deps/dpdk-25.11.3/install/lib/x86_64-linux-gnu:$PWD/deps/dpdk-25.11.3/install/lib
taskset -c 2,4 build/perf-release/core/handoff_bench --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true --benchmark_min_time=0.3s \
    --benchmark_filter='BM_RoundTrip_(ptr_ring|elem|cached)'
```

Families: `BM_RoundTrip_*` (one burst in flight: a handoff and its resume, per burst
and per packet), `BM_OneWay_*` (producer and consumer; the second argument is the
consumer's per-burst delay in `pause` loops: 0 is balanced, 400 is a producer
that outruns its consumer and reports the refused fraction and the cost of a
producer-loop iteration when the queue is full), `BM_RefuseFull_*` (the cost of a
refused call, one thread) and `BM_PacketRoundTrip_*` (both sides touch the
packet, as a real slow path does, so that where the context lives is priced
honestly). Every run first moves a verified stream; a spin that makes no progress
for 20 s aborts. Counters: `ns_per_burst`, `ns_per_item`, `bytes_per_item` (ring
element), `ring_bytes`. For a paired A/B between two rows of one binary use
`tools/ab_bench.py` with `--filter-b` and `--rename-b`.

## Classified live perf tests (`tools/live_perf.py`)

A sample pipeline says that something got slower, not what. `tools/live_perf.py` runs small pipelines on one
pinned worker, each isolating one cost, and reports ns per packet from the leaf traffic classes' counters
(nothing added to the packet path):

- **F, framework (cross-cutting):** `F.floor` (Source -> Sink), `F.hop/N` (N Bypass hops; the slope is the cost
  of one module hop, including the Track hook every output gate has by default), `F.hopnt/16` (the same with
  those hooks removed: the hop alone), `F.fanout/4`, `F.fanin/4`, `F.hook` (Track hooks on input gates too),
  `F.tc/N` (scheduler with N leaves), `F.size/1500`, `F.queue` (cross-worker Queue).
- **M, modules (localised):** each module after the same Source -> Rewrite -> RandomUpdate (1,024 flows) front
  end, reported as ns per packet above `M.base` (the front end with a Bypass).

If `F.floor` or the `F.hop` slope moves, every pipeline moves: look at the scheduler, Source/Sink, the packet
pool and batch dispatch. If one `M` delta moves alone, the cost is in that module.

    omarchy-benchmark --isolate --diagnose --cpu 2,4,6 -- taskset -c 2,4,6 tools/live_perf.py \
        --cpu 4,6 --control-cpu 2 --counters \
        --build master --build current:<tree> --rounds 8 --save run.json
    tools/live_perf.py --build current --tests F --baseline run.json --threshold 5   # exits 1 on a regression

A build may also name its build directory and bessd flags, comma-separated: `current:<tree>:<builddir>:<flags>`.
The same binary under different flags is then two builds, paired like any others (D-126 compared
`current:<tree>:build/fast` with `current:<tree>:build/fast:--simd_width=256`).

Give every thread that is not a worker its own isolated CPU, the lowest of the set, and put the workers above it.
The isolated partition holds only what the wrapper starts, but inside it the kernel balances threads freely.
bessd keeps its control threads (main, gRPC, DPDK service threads) off worker CPUs (D-027), within its starting
CPU set. master pins them all to its EAL main lcore, which is the lowest CPU it may use. `--control-cpu` moves any
daemon thread still allowed on several CPUs onto that CPU once the daemon is up. With the workers on 2 and 4 (the
old recipe), master ran its gRPC threads on worker 0's CPU. `--diagnose` reports IRQ and softirq activity on the
set and allows the perf counters (`--counters`) for the run. The wrapper takes the SMT siblings (here 3, 5 and 7)
offline and restores everything on exit.

CI runs it report-only: the gcc lane, on pushes and the nightly run, runs one round of every F and M test on its own
build (`ci_profile.py live-perf`) and uploads `live-perf.json` as the artifact `live-perf-<sha>`. A shared runner is
not an isolated machine, and the lane is not a release build, so compare runs with each other, never with a local
baseline; decisions still come from isolated paired runs.

Builds must be release builds with `NDEBUG` (`b_ndebug=if-release`): a tree configured with `b_ndebug=false`
pays `_GLIBCXX_ASSERTIONS` everywhere (MODERNIZATION.md entry 148).

## Compiler tuning matrix (D-127)

`-march=x86-64-v3` release trees of one source (`-Dbuildtype=release`, `b_staticpic=false`, DPDK 25.11.3 shared,
linked with mold), one per `-Dtune`, measured in one live_perf run each: 8 palindromic rounds, workers on P-cores
4 and 6, control on 2, under `omarchy-benchmark --isolate --diagnose` (i9-13900H; GCC's `-march=native` tunes for
`alderlake`, the native column). Cells: median worker cycles per packet [min–max over the rounds] / median
instructions per packet, then the paired median cycles ratio against the first column and the rounds in which the
tune was faster. Absolute cycles of the whole pipeline (M rows include M.base). Raw data:
`docs/baselines/d127-tune-{gcc,gcc-floor,clang}.json`.

GCC 16.2.1 (the release compiler's family; first column = the default):

| test | generic | graniterapids | znver5 | alderlake (native) |
|---|---:|---:|---:|---:|
| F.floor | 7.3 [7.3–14.0] / 34.8 | 7.3 [7.3–14.8] / 34.8 (+0.2%, 3/8) | 13.4 [7.2–14.8] / 34.7 (+42.7%, 3/8) | 7.3 [7.3–7.3] / 35.0 (-0.3%, 5/8) |
| F.hop/16 | 34.9 [33.7–39.7] / 108.8 | 34.7 [34.0–45.0] / 111.4 (+0.5%, 4/8) | 36.6 [33.0–40.7] / 109.3 (+0.2%, 4/8) | 34.3 [33.2–35.1] / 109.0 (-1.5%, 5/8) |
| M.base | 25.8 [25.3–32.6] / 119.6 | 26.4 [25.3–34.9] / 119.7 (+2.3%, 3/8) | 28.4 [25.0–34.1] / 117.7 (+1.5%, 4/8) | 25.1 [24.8–39.6] / 117.9 (-2.5%, 5/8) |
| M.exactmatch | 57.6 [56.9–63.3] / 286.5 | 58.4 [57.6–65.9] / 287.4 (+2.4%, 0/8) | 59.4 [58.8–70.7] / 284.7 (+4.0%, 0/8) | 58.0 [57.7–67.5] / 283.8 (+1.5%, 1/8) |
| M.wildcardmatch | 73.9 [72.7–75.0] / 360.1 | 71.7 [70.1–78.4] / 357.9 (-2.8%, 6/8) | 72.5 [71.4–79.6] / 357.9 (-1.1%, 4/8) | 74.7 [73.9–82.4] / 355.3 (+1.9%, 2/8) |
| M.iplookup | 32.4 [31.1–37.4] / 154.1 | 31.3 [30.9–40.1] / 154.1 (-1.2%, 5/8) | 30.9 [30.6–37.4] / 152.0 (-3.4%, 6/8) | 31.0 [30.4–34.7] / 152.2 (-4.3%, 5/8) |
| M.metadata | 39.4 [37.9–51.8] / 178.1 | 39.6 [38.1–65.1] / 169.8 (+2.2%, 3/8) | 38.0 [37.2–46.3] / 176.1 (-3.4%, 6/8) | 38.3 [37.6–42.4] / 176.4 (-2.8%, 6/8) |

F.floor is bimodal in every tree: a fresh bessd runs it at about 7.3 or about 13–15 cycles per packet, with the
same instruction count. The znver5 median above is that (5 of its 8 daemons); a 16-round rerun of F.floor,
M.base and M.exactmatch put every tune at about 7.3 (2–4 slow daemons each), M.base at -3.3% (znver5) and -3.0%
(alderlake), and M.exactmatch again slower than generic in every tune (+1.6% to +3.8%; 3–4 of 16 rounds faster).

Clang 22.1.8 (first column = the default, `-mtune` unset, which is Haswell's model):

| test | default (haswell) | generic | graniterapids | znver5 | alderlake |
|---|---:|---:|---:|---:|---:|
| F.floor | 11.6 [11.5–12.0] / 36.9 | 11.3 [11.1–11.5] / 37.6 (-3.6%, 7/8) | 11.8 [11.6–12.0] / 36.7 (+1.3%, 2/8) | 10.5 [10.2–10.7] / 36.4 (-10.4%, 8/8) | 11.8 [11.5–12.0] / 36.7 (+0.9%, 3/8) |
| F.hop/16 | 41.6 [40.0–46.9] / 104.4 | 41.5 [40.6–43.2] / 105.1 (-0.6%, 5/8) | 40.9 [40.6–42.3] / 105.8 (-0.6%, 4/8) | 39.6 [38.4–42.9] / 104.0 (-5.2%, 7/8) | 42.4 [41.2–43.9] / 105.8 (+1.1%, 4/8) |
| M.base | 38.4 [37.0–42.1] / 123.0 | 37.4 [36.5–40.9] / 124.8 (-2.8%, 5/8) | 36.9 [36.0–38.4] / 122.8 (-4.5%, 8/8) | 35.4 [35.1–37.9] / 121.8 (-6.2%, 7/8) | 37.5 [36.2–41.3] / 122.9 (-2.8%, 5/8) |
| M.exactmatch | 69.5 [68.2–74.0] / 285.3 | 68.3 [68.0–70.5] / 286.8 (-1.3%, 7/8) | 71.0 [70.3–73.7] / 284.3 (+2.8%, 3/8) | 66.9 [65.7–69.2] / 283.8 (-3.9%, 7/8) | 69.5 [68.4–73.6] / 284.4 (+0.7%, 3/8) |
| M.wildcardmatch | 96.2 [94.6–98.3] / 389.9 | 91.5 [90.3–93.9] / 389.2 (-4.4%, 8/8) | 92.7 [91.6–95.3] / 384.8 (-3.3%, 8/8) | 90.4 [89.4–93.2] / 383.0 (-6.4%, 8/8) | 92.5 [91.9–96.0] / 385.0 (-3.3%, 7/8) |
| M.iplookup | 42.7 [42.3–44.5] / 153.5 | 43.3 [42.6–48.0] / 156.7 (+1.0%, 3/8) | 46.2 [45.0–51.6] / 142.5 (+8.3%, 0/8) | 41.6 [40.8–44.5] / 151.2 (-2.1%, 6/8) | 47.1 [46.5–48.7] / 142.5 (+10.2%, 0/8) |
| M.metadata | 53.9 [53.2–57.8] / 188.6 | 56.0 [55.3–62.4] / 193.5 (+3.5%, 0/8) | 56.7 [56.0–67.2] / 187.1 (+5.3%, 1/8) | 54.1 [52.9–57.4] / 186.2 (+0.1%, 3/8) | 55.0 [53.3–56.2] / 187.2 (+1.6%, 3/8) |

bessd text segment in bytes (`size`; the GCC files are 10.0–10.5 MB, the Clang ones 8.5 MB):

| tune | GCC 16 | Clang 22 |
|---|---:|---:|
| default | 8,193,549 (generic) | 6,349,058 (haswell) |
| generic | 8,193,549 | 6,322,994 |
| graniterapids | 7,701,160 | 6,364,746 |
| znver5 | 7,736,334 | 6,388,859 |
| alderlake | 7,710,370 | 6,370,062 |

GCC's generic text is 6% larger than its three CPU tunes. Per object against alderlake, 442 KB of the growth is
control-plane, startup, protobuf and SDK code (`control_transaction`, `control_pipeline_spec`,
`stats_metric_registry`, `runtime_startup`, ...) and 41 KB is modules (`exact_match` +9 KB, `ip_lookup` +5 KB,
`wildcard_match` +3 KB); Source, Sink, Bypass and Rewrite are within 50 bytes.
