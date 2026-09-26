# Benchmarking BESS

How to produce numbers that can be trusted and compared. Decisions in
[decisions.md](decisions.md) cite benchmarks run this way (D-016).

## Build

- Benchmark a **release** build compiled for the machine you measure on:
  `meson setup build-bench -Dcpu=native --buildtype=release`.
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
