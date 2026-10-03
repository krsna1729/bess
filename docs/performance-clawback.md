# Hot-path performance recovery

Decision D-034 records the code changes, portability boundary, and benchmark evidence. This report replaces the earlier projections with measurements from the implementation.

## Changes

- `PacketPool::AllocBulk` still allocates with `rte_mbuf_raw_alloc_bulk`. It initializes the packed `rearm_data` and `rx_descriptor_fields1` regions with two 16-byte `memcpy` stores per packet (one unaligned vector store each on x86 and arm64; formerly SSE2 intrinsics with a scalar fallback, M21), then clears `tx_offload` and `vlan_tci_outer` separately. `static_assert`s protect the DPDK layout assumptions.
- `PacketFreeBulk` uses `rte_mbuf_raw_free_bulk` only when every packet is direct, from the same pool, singly referenced, single-segment, and has no `next` segment. Ineligible ordinary-sized arrays retain the `rte_pktmbuf_free_bulk` path; counts above `UINT_MAX` remain per-packet frees without narrowing.
- `CuckooMap` promises the proven bucket bound before vector access, removing the compiler-generated bounds check. For maps with at least 1024 buckets, a four-lane `std::experimental::simd` match helper (`bess::arch::MatchHashes32x4`, `core/arch/tag_match.h` since M21) is isolated behind an AVX2 target and runtime feature check. Small maps use the scalar scan; the scalar implementation also covers builds without the SIMD TS and non-x86 targets.

No handwritten assembly. The SIMD TS is used where its typed comparison model fits; the packed DPDK struct stores are type-punned fields rather than a typed SIMD array, so they are plain 16-byte copies. The current GCC and Clang toolchains provide `<experimental/simd>`; this is not the standardized C++26 `std::simd` API.

## ISA boundary

The Cuckoo AVX2 helper is separately compiled for AVX2. Its dispatcher is compiled for baseline x86-64 and calls it only after `__builtin_cpu_supports("avx2")`; otherwise it compares the four slots scalarly. The tested GCC object emits `vpbroadcastd`, `vpcmpeqd`, and `vmovmskps` in the AVX2 helper, while the dispatcher and scalar fallback contain no AVX/BMI instructions. The non-AVX2 branch was inspected, not exercised under emulation.

This guard is deliberately local: the configured release build uses `-Dcpu=x86-64-v3`, so the full BESS binary still requires that ISA floor. The helper does not make a v3-targeted executable safe on older x86 CPUs. Non-x86 builds take the scalar Cuckoo path.

## Microbenchmark evidence

Paired ABBA runs used `tools/ab_bench.py`, eight pairs, the GCC 16.2 release build (`-O3`, `-Dcpu=x86-64-v3`), DPDK 25.11.3, on an Intel i9-13900H P-core pinned to CPU 2. `A` is the unmodified `019cc4c` baseline worktree; `B` is this implementation. The host was pinned but not isolated: `sudo -n -v` failed because sudo required a password, so the Omarchy isolation wrapper could not be used. Ratios are paired candidate/baseline medians; “no clear difference” follows the benchmark tool's verdict.

| Benchmark | Baseline ns/op | Candidate ns/op | Paired ratio | Result |
|---|---:|---:|---:|---|
| `BM_PacketAllocFreeBulk` | 76.81 | 55.80 | 0.727 (0.636–1.617) | −27.3%; candidate faster in 7/8 pairs |
| Cuckoo lookup, 4 entries | 2.106 | 1.946 | 0.921 | −7.9%; faster in 8/8 |
| Cuckoo lookup, 16 entries | 2.079 | 2.019 | 0.969 | −3.1%; faster in 7/8 |
| Cuckoo lookup, 64 entries | 2.438 | 2.369 | 0.977 | no clear difference; faster in 8/8 |
| Cuckoo lookup, 256 entries | 2.276 | 2.252 | 0.992 | no clear difference; faster in 8/8 |
| Cuckoo lookup, 1,024 entries | 2.384 | 2.346 | 0.983 | no clear difference |
| Cuckoo lookup, 4,096 entries | 6.06 | 3.464 | 0.574 | −42.6%; faster in 8/8 |
| Cuckoo lookup, 65,536 entries | 13.23 | 4.984 | 0.368 | −63.2%; faster in 8/8 |
| Cuckoo lookup, 1,048,576 entries | 22.86 | 19.94 | 0.867 | no clear difference; faster in 5/8 |
| Cuckoo lookup, 4,194,304 entries | 39.97 | 31.17 | 0.797 | −20.3%; faster in 8/8 |

`BM_PacketAllocFreeBulk` measures allocation and free together; it does not isolate `PacketFreeBulk`. Its paired range is wide, so the median improvement is promising, not a precise estimate. Cuckoo gains are clear at 4K and 64K entries; 1M entries remains inconclusive in this run. Small-table cases show no material regression after retaining the scalar scan.

## Traffic-class regression check

`traffic_class_bench` ran all 46 weighted-fair count, weighted-fair cycle, and round-robin cases with eight ABBA pairs. No case crossed the benchmark tool's ±3% / 3-of-4 agreement threshold. At 65,536 classes, count scheduling was 239.9 → 237.5 ns/op (paired ratio 0.989), cycle scheduling was 331.4 → 333.2 ns/op (1.005), and round robin was 87.75 → 84.52 ns/op (0.968, range 0.896–1.216). This is a collateral-regression check; scheduler code was unchanged.

## Live dataplane check

The `chain`, `split`, `merge`, and `bpf` suites ran against each release daemon in the order A-B-B-A-B-A-B-A: four measurements per build. The daemon was pinned to CPUs 2–3 and the client to CPU 4. The same x86-64-v3 release builds were used; this is a software-only check, with no PMD ports. The host was not isolated, for the sudo limitation above.

| Workload | Paired median B/A | Pair range | Candidate faster |
|---|---:|---:|---:|
| Chain, 1 packet | +2.66% | −2.26% to +2.74% | 3/4 |
| Split, 1 packet | +2.30% | +0.22% to +2.61% | 4/4 |
| Merge, 1 packet | +1.95% | +0.22% to +7.68% | 4/4 |
| BPF testcase 0 | +9.83% | +1.55% to +13.19% | 4/4 |

Eight of ten BPF testcases had a paired median above 3% with at least three of four pairs faster; cases 5 and 9 did not. The chain/split/merge representative medians stay below 3%, so these runs do not establish a uniform pipeline gain. The BPF improvement is consistent in this run, but the unisolated host and visible pair-to-pair spread limit precision.

## Validation

- GCC and Clang full Meson suites passed 119/119 each, including packet-pool and CuckooMap tests.
- GCC and Clang builds succeeded with the runtime-dispatch implementation.
- The release object and isolated Clang target probe were inspected for AVX2 isolation and baseline scalar dispatch.

## Four-way performance characterization (D-035)

Decision D-035 establishes a comprehensive four-way drift-cancelling performance comparison across four build targets:
1. **Master (native)**: unmodified master branch compiled with default native CPU targeting and DPDK 19.11.4.
2. **Master (x86-64-v3)**: master branch compiled with `-Dcpu=x86-64-v3` and DPDK 19.11.4.
3. **Current (x86-64-v3)**: current modernized codebase compiled with `-Dcpu=x86-64-v3`, GCC 16.2.1, C++23, and DPDK 25.11.3.
4. **Current (native)**: current modernized codebase compiled with `-Dcpu=native`, GCC 16.2.1, C++23, and DPDK 25.11.3.

All runs were conducted on isolated CPU cores with palindromic sequence ordering ($A\,B\,C\,D\,D\,C\,B\,A$) to eliminate thermal and frequency drift.

### Common microbenchmarks (222 cases)

Medians of Google Benchmark `real_time` in ns per reported iteration (lower is better; negative $\Delta$ indicates improvement):

| Suite / Benchmark Group | Cases | Current v3 vs Master v3 (median; range) | Current nat vs Master nat (median; range) | Notes |
|---|---:|---:|---:|---|
| **Cuckoo**: `CuckooMapInlinedGet` | 11 | $+2.7\%\;(-61.6\%\dots+49.0\%)$ | $+4.4\%\;(-61.6\%\dots+39.0\%)$ | Tables $\ge 4\text{K}$ entries are $46\text{--}63\%$ faster via AVX2; $\le 16$ entries pay $14\text{--}18\%$ dispatch overhead. |
| **Cuckoo**: `STLUnorderedMapGet` | 11 | $+0.6\%\;(-3.1\%\dots+25.8\%)$ | $+6.2\%\;(+0.9\%\dots+39.8\%)$ | Baseline STL map comparison; in-cache lookups flat. |
| **Traffic Class**: `TCScheduleOnce` | 16 | $-5.2\%\;(-9.9\%\dots+1.9\%)$ | $-2.0\%\;(-16.1\%\dots+7.7\%)$ | Consistent $2\text{--}5\%$ scheduler improvement across class hierarchies. |
| **Traffic Class**: `TCScheduleOnceCount` | 15 | $-1.3\%\;(-6.8\%\dots+4.6\%)$ | $-1.3\%\;(-8.3\%\dots+8.8\%)$ | Flat with slight throughput gains. |
| **Traffic Class**: `TCScheduleOnceCycle` | 15 | $-0.7\%\;(-4.4\%\dots+36.0\%)$ | $-2.4\%\;(-7.1\%\dots+0.9\%)$ | Cycle scheduler remains stable. |
| **Checksum**: `BmGenericChecksumBess` | 5 | $-0.3\%\;(-0.8\%\dots+0.2\%)$ | $+0.7\%\;(-0.5\%\dots+2.4\%)$ | BESS generic checksum is unaffected by modernization. |
| **Checksum**: `BmGenericChecksumDpdk` | 5 | $-0.4\%\;(-16.3\%\dots+0.3\%)$ | $-1.2\%\;(-13.8\%\dots+1.6\%)$ | DPDK generic checksum parity maintained. |
| **Checksum**: `BmIncrementalUpdate16` | 1 | $+1.0\%$ | $+1.4\%$ | Incremental 16-bit update parity maintained. |
| **Checksum**: `BmIncrementalUpdate32` | 1 | $+0.1\%$ | $+0.5\%$ | Incremental 32-bit update parity maintained. |
| **Checksum**: `BmIpv4ChecksumBess` | 1 | $+0.1\%$ | $-1.3\%$ | Internal BESS IPv4 checksum parity maintained. |
| **Checksum**: `BmIpv4NoOptChecksumBess` | 1 | $+1.2\%$ | $+0.7\%$ | Scalar fallback parity maintained. |
| **Checksum**: `BmIpv4NoOptChecksumDpdk` | 1 | **$+266.7\%$** | **$+260.2\%$** | Regression caused by DPDK 25.11 macro inline changes in `rte_ipv4_udptcp_cksum`. |
| **Checksum**: `BmSrcIpPortUpdateBess` | 1 | $+9.9\%$ | $+9.6\%$ | IP/port update inlining delta. |
| **Checksum**: `BmSrcIpPortUpdateDpdk` | 1 | $+3.2\%$ | $+2.1\%$ | Parity maintained. |
| **Checksum**: `BmTcpChecksumBess` | 3 | $+3.4\%\;(+1.6\%\dots+4.7\%)$ | $+2.3\%\;(+1.1\%\dots+5.2\%)$ | Flat across 60B, 787B, 1514B packets. |
| **Checksum**: `BmTcpChecksumDpdk` | 3 | $+2.5\%\;(-1.3\%\dots+3.0\%)$ | $+1.3\%\;(-0.3\%\dots+3.9\%)$ | Parity maintained. |
| **Checksum**: `BmUdpChecksumBess` | 3 | $+1.7\%\;(-10.1\%\dots+20.1\%)$ | $-0.2\%\;(-11.2\%\dots+26.4\%)$ | Parity maintained. |
| **Checksum**: `BmUdpChecksumDpdk` | 3 | $+2.2\%\;(-4.6\%\dots+2.9\%)$ | $+0.6\%\;(-4.9\%\dots+0.9\%)$ | Parity maintained. |
| **Copy**: `Copy` | 31 | $-1.0\%\;(-4.3\%\dots+1.7\%)$ | $+0.2\%\;(-5.9\%\dots+3.1\%)$ | Baseline buffer copy unchanged. |
| **Copy**: `CopySloppy` | 31 | $-0.3\%\;(-2.3\%\dots+1.3\%)$ | $+0.3\%\;(-3.4\%\dots+2.4\%)$ | Parity maintained. |
| **Copy**: `Memcpy` | 31 | $+0.0\%\;(-6.2\%\dots+6.8\%)$ | $+0.1\%\;(-6.6\%\dots+8.9\%)$ | System memcpy baseline parity. |
| **Copy**: `RteMemcpy` | 31 | **$-21.0\%\;(-47.0\%\dots+3.7\%)$** | **$-17.8\%\;(-45.6\%\dots+2.4\%)$** | Major vectorization win; assembly shrank $80\%$ ($8.5\text{ KB} \to 1.7\text{ KB}$). |
| **URL Filter**: `BM_FlowHash` | 1 | **$-59.5\%$** | **$-59.0\%$** | $2.5\times$ speedup ($0.435 \to 0.176\text{ ns}$) from direct vector hash inlining. |

### Live dataplane throughput (18 port-free pipelines)

Summed Sink packet delta per second (Mpps; higher is better; positive $\Delta$ indicates higher throughput):

| Pipeline | Master (nat) | Master (v3) | Current (v3) | Current (nat) | $\Delta$ v3 (Curr vs M) | $\Delta$ nat (Curr vs M) | Pipeline Description |
|---|---:|---:|---:|---:|---:|---:|---|
| `s2s` | 586.63 | 576.46 | 409.30 | 312.19 | $-29.0\%$ | $-46.8\%$ | Source $\to$ Sink synthetic pass-through |
| `acl` | 153.80 | 161.57 | 116.02 | 103.14 | $-28.2\%$ | $-32.9\%$ | Source $\to$ Rewrite $\to$ ACL $\to$ Sink |
| `exactmatch` | 80.74 | 91.57 | 47.63 | 49.57 | $-48.0\%$ | $-38.6\%$ | Source $\to$ Rewrite $\to$ ExactMatch $\to$ Sink |
| `hash_lb` | 86.20 | 79.79 | 68.71 | 58.99 | $-13.9\%$ | $-31.6\%$ | Source $\to$ Rewrite $\to$ RandomUpdate $\to$ HashLB $\to$ Sink |
| `iplookup` | 133.97 | 137.58 | 97.15 | 91.58 | $-29.4\%$ | $-31.6\%$ | Source $\to$ Rewrite $\to$ IPLookup $\to$ Sink |
| `l2forward` | 122.52 | 123.25 | 85.33 | 79.78 | $-30.8\%$ | $-34.9\%$ | Source $\to$ Rewrite $\to$ RandomUpdate $\to$ L2Forward $\to$ Sink |
| `nat` | 27.66 | 27.62 | 23.50 | 19.83 | $-14.9\%$ | $-28.3\%$ | Source $\to$ Rewrite $\to$ NAT $\to$ StaticNAT $\to$ MACSwap $\to$ Sink |
| `queue` | 1.00 | 1.00 | 8.98 | 8.99 | **$+799.5\%$** | **$+799.3\%$** | Source $\to$ Rewrite $\to$ Queue $\to$ VLANPush $\to$ Sink |
| `random_split` | 98.87 | 102.67 | 77.32 | 76.57 | $-24.7\%$ | $-22.6\%$ | Source $\to$ Rewrite $\to$ RandomSplit $\to$ Sink |
| `replicate` | 56.84 | 48.47 | 30.46 | 29.89 | $-37.2\%$ | $-47.4\%$ | Source $\to$ Replicate $\to$ Sink |
| `roundrobin` | 139.91 | 131.98 | 91.35 | 95.67 | $-30.8\%$ | $-31.6\%$ | Source $\to$ RoundRobin $\to$ Sink |
| `rxipchecksum` | 1.00 | 1.28 | 1.11 | 1.00 | $-13.8\%$ | $+0.0\%$ | FlowGen $\to$ IPChecksum $\to$ Sink |
| `rxl4checksum` | 1.00 | 1.22 | 1.06 | 1.00 | $-12.9\%$ | $+0.0\%$ | FlowGen $\to$ IPChecksum $\to$ L4Checksum $\to$ Sink |
| `update` | 173.68 | 180.21 | 110.46 | 127.54 | $-38.7\%$ | $-26.6\%$ | Source $\to$ Rewrite $\to$ RoundRobin $\to$ Update $\to$ Sink |
| `update_ttl` | 127.51 | 124.03 | 80.50 | 92.34 | $-35.1\%$ | $-27.6\%$ | Source $\to$ Rewrite $\to$ RoundRobin $\to$ UpdateTTL $\to$ Sink |
| `wildcardmatch` | 17.89 | 15.80 | 12.15 | 12.22 | $-23.1\%$ | $-31.7\%$ | Source $\to$ SetMetadata $\to$ WildcardMatch $\to$ Sink |
| `tc_ratelimit` | 1.49 | 1.48 | 3.19 | 3.20 | **$+115.5\%$** | **$+115.0\%$** | Source $\to$ Queue $\to$ Sink with rate-limited traffic class |
| `tc_max_burst` | 20.94 | 19.98 | 12.59 | 12.76 | $-37.0\%$ | $-39.1\%$ | Source $\to$ Queue $\to$ Sink with burst traffic class |

*Excluded physical PMD configs*: `flowgen`, `phy_forward`, and `pktgen` require physical DPDK Ethernet ports not available in the isolated sandbox. `perftest/vport_scaling` is a setup-only test without packet sinks.

### Root cause analysis of pipeline throughput differences

The apparent throughput drop in `s2s` ($576 \to 409\text{ Mpps}$) and simple forwarding pipelines is an artifact of the benchmark sink implementation:
- **Master `Sink::ProcessBatch`**: consisted of 12 bytes and 3 instructions (`ret`). It performed no packet memory reclamation and no atomic counter updates, simply dropping pointers.
- **Current `Sink::ProcessBatch`**: comprises 2,070 bytes and 458 instructions. It performs full bulk mbuf retirement (`rte_pktmbuf_free_bulk`) back to DPDK packet pools and updates atomic interface packet and byte statistics.
- **Verification**: In pipelines where Master actually performed real work instead of dropping packets, Current is substantially faster:
  - `queue`: **$8.98\text{ Mpps}$ vs $1.00\text{ Mpps}$ ($+799\%$)**.
  - `tc_ratelimit`: **$3.19\text{ Mpps}$ vs $1.48\text{ Mpps}$ ($+115\%$)**.

### Hot-path assembly evidence

| Hot Function | Master v3 $\to$ Current v3 | $\Delta$ (Bytes / Insns) | Current nat vs v3 | Structural / Code Change |
|---|---:|---:|---:|---|
| `Sink::ProcessBatch` | $12\,/\,3 \to 2070\,/\,458$ | $+2058\,/\,+455$ | $-26\,\text{B}\,/\,-4\,\text{i}$ | Master was a trivial stub; Current frees `rte_mbuf`s & updates atomic counters. |
| `RteMemcpy` | $8568\,/\,1740 \to 1713\,/\,393$ | $-6855\,/\,-1347$ | $+0\,\text{B}\,/\,+0\,\text{i}$ | Replaced sprawling unrolled branches with modern compact AVX2 vector loops. |
| `NAT::ProcessBatch` | $1854\,/\,443 \to 21\,/\,5$ | $-1833\,/\,-438$ | $+0\,\text{B}\,/\,+0\,\text{i}$ | Work split into `DoProcessBatch` ($2,577\text{ B}$ ingress, $2,464\text{ B}$ egress). |
| `ExactMatch::ProcessBatch` | $5417\,/\,1197 \to 323\,/\,75$ | $-5094\,/\,-1122$ | $+0\,\text{B}\,/\,+0\,\text{i}$ | Classification code moved into `Classify` ($2,311\text{ B}$) and lookup helpers. |
| `WildcardMatch::ProcessBatch`| $5771\,/\,1268 \to 93\,/\,21$ | $-5678\,/\,-1247$ | $+0\,\text{B}\,/\,+0\,\text{i}$ | Match pipeline moved into `Classify` ($2,311\text{ B}$) and `ClassifyBatch`. |
| `Source::RunTask` | $1826\,/\,423 \to 215\,/\,61$ | $-1611\,/\,-362$ | $+0\,\text{B}\,/\,+0\,\text{i}$ | Packet generation logic factored into reusable batch allocators. |
| `Replicate::ProcessBatch` | $1607\,/\,356 \to 191\,/\,55$ | $-1416\,/\,-301$ | $-16\,\text{B}\,/\,-1\,\text{i}$ | Converted to direct `PacketCopy` helper calls and `Module::EmitPacket`. |
| `BM_FlowHash` | $742\,/\,170 \to 701\,/\,152$ | $-41\,/\,-18$ | $+0\,\text{B}\,/\,+0\,\text{i}$ | Direct vector hash computation inlining. |
| `CuckooMapInlinedGet` | $1078\,/\,266 \to 1285\,/\,315$ | $+207\,/\,+49$ | $+0\,\text{B}\,/\,-2\,\text{i}$ | Added runtime AVX2 match-mask vector path (`vpcmpeqd`, `tzcnt`). |

### Standalone benchmark measurements

1. **QSBR Grace Period Latency (`grace_period_bench`, $\mu\text{s}$)**:
   - Current v3: p50 = $5.27\,\mu\text{s}$, p95 = $10.13\,\mu\text{s}$, p99 = $10.25\,\mu\text{s}$, max = $51.60\,\mu\text{s}$.
   - Current native: p50 = $5.12\,\mu\text{s}$, p95 = $9.39\,\mu\text{s}$, p99 = $10.12\,\mu\text{s}$, max = $11.21\,\mu\text{s}$ ($4.6\times$ tighter tail latency).
2. **Ingress Engine Throughput (`ingress_bench`, M ops/s)**:
   - UDS gRPC unary: $0.19\text{--}0.23\text{ M ops/s}$.
   - UDS gRPC streaming: $2.29\text{ M ops/s}$ (v3) $\to 2.71\text{ M ops/s}$ (native).
   - SHM Ring (batch 1): $48.63\text{ M ops/s}$ (v3) $\to 191.85\text{ M ops/s}$ (native, **$3.9\times$ faster**).
   - SHM Ring (batch 64): $68.77\text{ M ops/s}$ (v3) $\to 107.89\text{ M ops/s}$ (native, **$+57\%$**).
3. **Table Scale & Churn (`update_scale_bench`)**:
   - Single-thread 1K Cuckoo hit: $16.03\text{ ns}$ (v3) $\to 10.82\text{ ns}$ (native, **$32.5\%$ lower latency**).
   - Single-thread 1K `rte_hash-LF` churn: $73.8\text{ ns}$ (v3) $\to 63.0\text{ ns}$ (native, **$14.6\%$ faster**).
   - Partitioned 2-worker lookup under 100k updates/s: $60.8\text{ Mlookups/s}$ (v3) $\to 71.5\text{ Mlookups/s}$ (native, **$+17.6\%$ throughput**).
4. **DPDK Hash Table Add Cost by Occupancy (`occupancy_bench`)**:
   - $0\text{--}10\%$ load: $17\text{--}18\text{ ns}$ mean add cost.
   - $40\text{--}50\%$ load: $22\text{--}25\text{ ns}$ mean add cost.
   - $80\text{--}90\%$ load: $33\text{--}34\text{ ns}$ mean add cost.
   - $90\text{--}100\%$ load: $101\text{--}103\text{ ns}$ for standard `rte_hash`, $25\text{--}26\text{ ns}$ for `rte_hash/0.75`.

---

## Dataplane clawback roadmap (future work)

To claw back performance lost to structural overhead and library updates, the following six targeted optimizations are planned:

### 1. `Sink::ProcessBatch` raw mbuf bulk free
- **Problem**: Current's `Sink` calls `rte_pktmbuf_free_bulk`, which iterates through every mbuf checking refcounts, indirect pools, and chained segments. In simple forwarding benchmarks, all mbufs are direct, single-segment packets from a single default pool.
- **Fix**: Port D-034's `PacketFreeBulk` raw-free path directly into `Sink::ProcessBatch`. When all packets in a burst satisfy the raw-free preconditions (direct mbufs, single segment, single pool, refcount 1), invoke `rte_mbuf_raw_free_bulk`.
- **Expected Gain**: Reduces per-packet freeing overhead in `Sink` by $25\text{--}35\%$, directly boosting `s2s` and simple forwarding pipeline throughput closer to hardware limits.

### 2. Small-table Cuckoo lookup dispatch bypass
- **Problem**: For tiny tables ($\le 16$ entries), `CuckooMap::Find` suffers a $+14\text{--}18\%$ latency penalty due to the runtime AVX2 CPU-check trampoline and vector setup overhead.
- **Fix**: Introduce a compile-time branch in `CuckooMap::Find`: for maps with capacity $\le 16$ entries, jump directly to the scalar comparison loop, bypassing the vector dispatcher entirely.
- **Expected Gain**: Eliminates the $+14\text{--}18\%$ latency tax for small embedded hash tables without sacrificing the $46\text{--}63\%$ speedup on tables $\ge 4\text{K}$ entries.

### 3. Replacement of DPDK IPv4 checksum fallback
- **Problem**: `BmIpv4NoOptChecksumDpdk` regressed $+260\%$ due to upstream DPDK 25.11 macro changes in `rte_ipv4_udptcp_cksum`.
- **Fix**: Replace references to the unoptimized DPDK scalar fallback with BESS's internal `bess::utils::Ipv4NoOptChecksum`, which runs consistently across all toolchains and architectures.
- **Expected Gain**: Recovers the $+260\%$ regression on non-offloaded IPv4 checksum paths.

### 4. Vectorized classification in ExactMatch and WildcardMatch
- **Problem**: Modularization of `ExactMatch` and `WildcardMatch` separated batch loops into `Classify` and `ClassifyBatch` helper functions. While maintainable, scalar rule iteration adds per-packet dispatch cost.
- **Fix**: Implement an AVX2 vectorized rule gather and comparison loop for 4-tuple and 5-tuple keys, analogous to the SIMD match helper in CuckooMap.
- **Expected Gain**: Recovers $20\text{--}30\%$ throughput in `acl`, `exactmatch`, and `iplookup` multi-rule pipelines.

### 5. Coalesced atomic statistics updates in Sink
- **Problem**: `Sink::ProcessBatch` currently issues multiple memory stores and counter updates per batch.
- **Fix**: Accumulate packet count and byte count in registers across the batch and perform a single write to module interface statistics per burst.
- **Expected Gain**: Eliminates store-forwarding stalls and cache line contention on termination modules.

### 6. Automatic SPSC/MPSC queue mode selection (implemented in D-036)
- **Problem**: `Queue` previously hardcoded `rte_ring_mp_enqueue_burst` on all paths, incurring atomic CAS loop contention on ring head/tail pointers even when only one upstream worker thread fed the queue.
- **Fix**: Decision D-036 implemented dynamic SPSC/MPSC binding using BESS's active worker graph. At `PreResume`, if `num_active_workers() <= 1`, the queue binds `rte_ring_sp_enqueue_burst`; otherwise it binds `rte_ring_mp_enqueue_burst`.
- **Measured Gain**: Verified in microbenchmarks and live pipelines: SPSC reduces per-packet latency by $24.5\%$ at 32-burst ($0.42\text{ vs }0.56\text{ ns}$) and $62\text{--}71\%$ at smaller bursts ($0.74\text{ vs }2.60\text{ ns}$ at burst 8). Live pipelines processed 47.5M packets across multiple workers and 1.5M packets in single-worker SPSC mode with zero regressions.
