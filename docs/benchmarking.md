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
  of one module hop), `F.fanout/4`, `F.fanin/4`, `F.hook` (Track hooks on every gate), `F.tc/N` (scheduler with
  N leaves), `F.size/1500`, `F.queue` (cross-worker Queue).
- **M, modules (localised):** each module after the same Source -> Rewrite -> RandomUpdate (1,024 flows) front
  end, reported as ns per packet above `M.base` (the front end with a Bypass).

If `F.floor` or the `F.hop` slope moves, every pipeline moves: look at the scheduler, Source/Sink, the packet
pool and batch dispatch. If one `M` delta moves alone, the cost is in that module.

    omarchy-benchmark --isolate --cpu 2,4 -- tools/live_perf.py \
        --build master --build current:<tree> --rounds 4 --save run.json
    tools/live_perf.py --build current --tests F --baseline run.json --threshold 5   # exits 1 on a regression

Builds must be release builds with `NDEBUG` (`b_ndebug=if-release`): a tree configured with `b_ndebug=false`
pays `_GLIBCXX_ASSERTIONS` everywhere (MODERNIZATION.md entry 148).
