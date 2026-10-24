# Benchmarking BESS

How to produce numbers that can be trusted and compared. Decisions in
[decisions.md](decisions.md) cite benchmarks run this way (D-016).

CI's nightly benchmark workflow measures a fixed subset at several `develop`
commits in one job and flags within-run regressions; see
[benchmark-trends.md](benchmark-trends.md) for the workflow, thresholds and
the `benchmarks` history branch. The rest of this document is for manual,
comparable A/B measurement.

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
- **Record the power profile beside the number.** On this host
  `powerprofilesctl set performance` moves these metrics 13-20%: the same
  release binary, ten fresh daemons each, gave F.floor 1.91 / F.hop/16 8.11 /
  M.base 6.80 under `balanced` against 1.65 / 6.87 / 5.93 under
  `performance`. A `performance` run is therefore not comparable to a
  `balanced` one, and the recorded baselines (D-127's tune matrix among them)
  were taken under the default profile. State `powerprofilesctl get` with any
  number you keep, and put the profile back afterwards.
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

`live_transaction_bench` (C++ on the SDK, built with the tests as
`build/<dir>/tests/perf/live_transaction_bench`; D-139) measures a running
bessd: packets classified by a real ExactMatch module on one worker while a
client changes its rules through the transaction RPC (D-025) at given rates. By
default each transaction adds one session's rule and removes the oldest;
`--upserts N --erases M` set that shape, where equal counts keep the table's
size fixed (which is what makes a sweep a slope, not table growth), and `--keep`
leaves the pipeline applied so another client can be measured against the same
modules and the same live traffic. Run interleaved with the Go SDK's own
benchmark (`livebench_test.go`) on one daemon, three ABBA rounds per shape, the
worker on a core of its own (`--worker-core 2`; with the default it shares CPU 0
with the daemon's control thread, which costs both clients about 3x on p99 and
halves the packet rate). Medians of six runs per client, with the range of those
runs in brackets: p50 at 2, 8 and 16 operations is C++ 111 (103-120), 161
(150-166) and 191.5 (185-240) us against Go 93 (84-100), 110 (106-111) and 136
(131-140) us. Ratios of 0.84, 0.68 and 0.71, or about 99 us fixed plus 5.8 us
per operation against 87 us plus 3.1 us. Nothing measured splits a per-operation
cost between the client, the wire and the daemon's table update, and one run per
client means little: the spread within a client reaches a quarter of the median
here. It prints
Mpps through the module, the hit fraction (about 0.5 while the live set stays
the shape's size, which checks that rules steer packets; a table that grows
shows more), transactions/s achieved, client-side
latency and the daemon's busy answers. Start bessd isolated on the worker core
as for any benchmark, run the tool from another core (`--grpc-url`,
`--worker-core`), and compare rates within one run (`--rounds` interleaves
them). `--worker-core auto` lets the daemon place the worker (D-123 order: a
free CPU, striped across the sockets). Before it measures, the tool prints the
daemon's own placement line and warns for any worker the daemon reports on a
shared physical core, on a control CPU, or off its memory node
(`control_v2.WorkerPlacement`, the resolver's verdict read back through
GetPipeline — the client derives no topology); `--allow-dirty-placement`
silences the warning. Numbers taken while siblings compete measure contention,
so treat a warned run as the caveat says.

## The firewall table

`classifier_firewall_table_bench` (`core/classifier/firewall_table_bench.cc`)
measures the table's packet path (`LookupBatch` over 32 keys) against the scalar
scan of the same rule list, and the masking kernel against the portable loop it
replaced. Build and run it from the release tree, pinned to one benchmark core
(`build/relsem` is a release lane with `-Dcpu=x86-64-v3` and benchmarks on):

```
ninja -C build/relsem core/classifier_firewall_table_bench
taskset -c 2 build/relsem/core/classifier_firewall_table_bench \
    --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

Args are `{rules, batch, dist, family}`: `dist` 0/1/2 is "every key hits the
first rule", "hits spread over the rules", "every key misses". Measured on this
host (release, `-march=x86-64-v3`, `balanced` profile, pinned to core 2, medians
of 3, cv <= 0.5%, and a desktop load of ~3 on 20 CPUs: the absolute values moved
about 20% between two such runs, so read the ratios and re-measure before
quoting a number):

| policy, 32 keys per batch | ns per batch | M keys/s |
|---|---|---|
| v4, 10 rules, first rule hit | 281 | 114 |
| v4, 1,000 rules, hits spread | 945 | 34 |
| v4, 1,000 rules, all miss | 709 | 45 |
| v4, 10,000 rules, hits spread | 1,349 | 24 |
| v6, 1,000 rules, hits spread | 1,243 | 26 |
| scalar scan, 10 rules, first rule hit | 274 | 117 |
| scalar scan, 1,000 rules, hits spread | 67,659 | 0.47 |

A handful of rules is cheaper to scan than to index - at 10 rules the scan wins
by 1.26x - and the index's advantage grows to 72x at 1,000. Building the table
(what an apply costs) is about 50 µs at 1K rules and about 1 ms at 10K
(`BM_FirewallBuild`; 44-58 µs and 0.82-1.07 ms across runs).

What a controller pays is `modules_firewall_update_bench`
(`core/modules/firewall_update_bench.cc`, D-179): one transaction through the
module's `<name>/rules` resource, `BM_RuleTransaction/<rules>/<ops>`, with the
operations decoded beforehand so only the apply is timed. It reports µs per
transaction and `busy`, the retries the engine asked for. The cost should
follow `rules` (one build per transaction) and barely move with `ops`; the
D-179 table is the reference.

`classifier_range_vs_firewall_bench`
(`core/classifier/range_vs_firewall_bench.cc`, D-186) puts one IPv4 5-tuple
policy into WildcardMatch's table with per-field ranges
(`ConcurrentMaskedTable`) and into `FirewallTable` (and `FirewallScan` up to
100 rules), and checks before timing that every key gets the same winner from
each. `BM_Lookup/<impl>/<rules>/<shape>/<dist>` cycles a 4,096-key stream
(`dist` 0 spread hits, 1 misses) or repeats one 32-key batch (`dist` 2, the
harness above: a best case, because the branch predictor learns the batch);
`BM_TableUpdate` and `BM_ModuleUpdate` time one rule added and removed, in the
table and through each module's `<name>/rules` resource. Run it as above:

```
ninja -C build/relsem core/classifier_range_vs_firewall_bench
taskset -c 2 build/relsem/core/classifier_range_vs_firewall_bench \
    --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

D-186 has the numbers and the tuple and group counts.

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

## Classified live perf tests (`tests/perf/live_perf`)

A sample pipeline says that something got slower, not what. `live_perf` (C++, a client of the C++ SDK over
control_v2; built with the tests as `build/<dir>/tests/perf/live_perf`) runs small pipelines on one pinned worker,
each isolating one cost, and reports ns per packet: the measured modules' output-gate packet counters (the Track
hook every connection has by default, read from `ListMetrics`) over a window the client times. Pipelines and table
entries go in as one `PipelineBuilder` transaction, the same path a controller uses.

- **F, framework (cross-cutting):** `F.floor` (Source -> Sink), `F.hop/N` (N Bypass hops; the slope is the cost
  of one module hop, including the Track hook every output gate has by default), `F.hopnt/16` (the same with
  those hooks removed: the hop alone), `F.fanout/4`, `F.split/2`, `F.fanin/4`, `F.tc/N` (scheduler with N
  leaves), `F.size/1500`, `F.queue` (cross-worker Queue), `F.flowgen` (FlowGen -> Sink: a source module, so
  `F.floor` is its control, not `M.base`).
- **M, modules (localised):** each module after the same Source -> Rewrite -> RandomUpdate (1,024 flows) front
  end, reported as ns per packet above `M.base` (the front end with a Bypass); `M.base`'s own row is the
  absolute baseline the others are measured from, so an arm and its control subtract to the module's cost.
- **M.gtpu_\* and M.urr_count\*, out-of-tree modules** (here the UPF plugin, krsna1729/upf `datapath/`):
  `--plugin DIR` appends `--modules=DIR` to the daemon command and enables them; without the flag they are
  skipped, so a lane without the plugin stays green. Each arm brings **its own control** (`M.gtpu_decap.base`,
  `M.gtpu_encap.base`, `M.urr_count.base`, `M.gtpu_decap.qfi.base`), with a Bypass in the module's slot so the
  delta is the module alone; read an arm and its control as a pair. The `M.gtpu_decap.qfi` pair carries the
  container frame and a reader of both `upf_teid` and `qfi`, so it is the one whose chain walk runs:
  (qfi - qfi.base) minus (decap - decap.base) is what the container adds to the decap. The GTP-U pair's control
  is the shipping graph's own front and
  tail -- the TEID classifier and its rule, the ActionId write, the tunnel record, the reader that keeps the
  decap's attribute real -- and its frames are the UPF smoke test's (`deploy/smoke-gtpu.yaml`), byte for byte:
  the numbers describe the graph that ships. The `M.urr_count` pair's is the same chain with the module
  swapped for the Bypass: SetMetadata writes the PDR rule's ActionId (D-141), a reader keeps the attribute
  real, and the frame is the chain the compiler writes a charged session into. Measuring a module that needs a
  tunnel frame against `M.base` would report the frame size, not the module.

If `F.floor` or the `F.hop` slope moves, every pipeline moves: look at the scheduler, Source/Sink, the packet
pool and batch dispatch. If one `M` delta moves alone, the cost is in that module. `live_perf --list` prints the
tests.

    omarchy-benchmark --isolate --diagnose --cpu 2,4,6 -- taskset -c 2,4,6 build/release/tests/perf/live_perf \
        --cpu 4,6 --control-cpu 2 --counters \
        --build current --build other=/tree/build/release/core/bessd --rounds 8 --save run.json
    build/release/tests/perf/live_perf --build current --tests F --baseline run.json --threshold 5   # exits 1 on a regression

`current` is the bessd next to the tool (`../../core/bessd`), and `live_perf`'s default builddir is
`build/perf-release`: measure there. `build/fast` is configured with meson buildtype `plain` plus fast.ini's
`-O1 -g0`, so a number from it is not comparable to the release baselines -- reading the two against each other
is how a 2x "regression" was once written up that was nothing but the build (F.floor read 3.7-4.0 on
`build/fast` against 1.91 on `build/perf-release`). A build may also name a tree, its build directory and
bessd flags, comma-separated: `current:<tree>:<builddir>:<flags>`, or a binary and flags: `name=<bessd>[,flag...]`.
The same binary under different flags is then two builds, paired like any others (D-126 compared
`current:<tree>:build/fast` with `current:<tree>:build/fast:--simd_width=256`). master is an archival snapshot of
upstream and is not a build `live_perf` drives; the master comparisons in D-092 to D-127 were made with the Python
tool it replaced (D-138).

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

`live_perf` reports the same verdict itself before it measures: the daemon's placement line and a warning for any worker it reports
on a shared physical core, on a control CPU, or off its memory node — once per distinct placement, since a round reuses the same
cores for every test — and `--allow-dirty-placement` silences it. A warned run still measures; the numbers carry the caveat.
`live_perf` and `update_scale_bench` keep their CPUs explicit — they pin their own threads and read perf counters — so their
`--cpu`/`--cpus` stay one CPU per physical core; only `live_transaction_bench` places workers automatically (see above).
`update_scale_bench` has no daemon to ask, so it reads the same resolver's topology (`bess::runtime::ReadTopology`,
`core/runtime/numa_placement.h`) and warns on shared physical cores among its own pinned CPUs: it has no worker
placement and no memory node to check.
`live_transaction_bench` additionally takes `--clients N` (N committers, one connection and one pacing thread
each; the report pools their latency, sums their rate and adds a `clients:` line, and each rate point prints the
client CPU it cost), `--daemon-pid` (the daemon's per-thread CPU per transaction, with the busy-polling workers
subtracted) and `--attach`/`--block-offset`/`--blocks-total` (several bench processes against one daemon, each
owning a slice of the rule space; the builder runs with `--keep`).

A bench run with `LD_LIBRARY_PATH` naming a stage (`build/stage-<lane>/usr/local/lib`) loads the **staged** `libbess-sdk.so`, not
the one in the build directory: after any change to the SDK's headers, re-install (`meson install -C build/<lane> --no-rebuild
--destdir build/stage-<lane>`) or the bench runs an old SDK against new headers, which is a crash, not a mismatch it can report.

Builds must be release builds with `NDEBUG` (`b_ndebug=if-release`): a tree configured with `b_ndebug=false`
pays `_GLIBCXX_ASSERTIONS` everywhere (MODERNIZATION.md entry 148).

## Noticing a counter threshold: the three paths

`stats_counter_paths_bench` (`core/stats/counter_paths_bench.cc`) measures what
each way of noticing that a per-key counter passed a limit costs: the packet
path's own counter write (`KeyedCounters::Plane::Add`, D-154), the one compare
the event path adds after it (`CrossingNotifier::Crossed`, D-152), the
crossing's `WorkerEvent` (`CrossingNotifier::Note` -> `EventHub::Post`, D-152),
arming K keys and sweeping them (`ThresholdTable::Sweep`, D-151), and a
controller reading every key (`Total`, the polling path). Build and run it from
a release tree, pinned to one benchmark core:

```
ninja -C build/fast core/stats_counter_paths_bench   # -Dbuild_benchmarks=true
taskset -c 2 build/fast/core/stats_counter_paths_bench \
    --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

Measured on this host (debugoptimized `-O2`, `-march=x86-64-v3`, pinned to core
2, medians of 3, cv <= 1.2%; this host has no isolated CPUs, so read the ratios
and re-measure before quoting a number):

| row | K = 1024 | K = 16384 | per unit |
|---|---|---|---|
| packet path: counter bump (`Plane::Add`) | 0.425 ns | 0.425 ns | per packet |
| packet path: bump + the crossing compare (`Crossed`) | 0.586 ns | 0.597 ns | per packet |
| packet path: the crossing (`Note` -> `EventHub::Post`) | 322 ns | -- | per crossing |
| control side: arm K keys | 23.4 us | 336.6 us | 22.8 / 20.5 ns per key |
| control side: poll every key (`Total`) | 684 ns | 10.88 us | 0.67 / 0.66 ns per key |
| control side: sweep every armed key, nothing crossing | 1.39 us | 22.44 us | 1.35 / 1.37 ns per key |
| control side: sweep every armed key, one entry firing | 1.73 us | 23.17 us | one K-entry walk, +0.35 us at K = 1024 for the fire and the erase |

The shape of the answer: the event path's per-packet tax in the steady state is
the compare, not nothing -- 0.59 ns against 0.425 ns for the bare bump, about
0.16 ns a packet, and `CrossingNotifier::Crossed` is `constexpr`. Its cost when
a key crosses is 322 ns, about 760 counter bumps, so the per-packet tax is that
322 ns divided by the packets a key takes to cross. The two control-side paths
read every key for about the price of a load: 0.67 ns per key to poll, 1.37 ns
per key to sweep (the sweep also compares against the limit and removes what
fired -- a firing entry adds 0.35 us to the 1024-key walk, not less: the library
walks every armed entry, and only `stop()` ends the walk early). 16,384 keys
cost 10.9 us to poll and 22.4 us to sweep, once per interval, against a
per-packet tax only the event path pays. Arming is a one-time 20-23 ns per key.

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
