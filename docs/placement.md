# CPU and NUMA placement

How bessd places its memory, its control threads and its workers (Decision
D-123, building on D-027). One rule everywhere: **bessd picks one memory node
at startup and everything it allocates lives there. Anything on another node
-- a worker's CPU, a NIC -- is diagnosed, never refused.** One bessd drives one
NUMA node; a multi-node host runs one bessd per node.

There is no bare-metal, container or VM detection. The resolver
(`core/runtime/numa_placement.{h,cc}`) has four inputs, and the environments
differ only in what they say:

| input | from | what it tells |
|---|---|---|
| sysfs | `/sys/devices/system/cpu/cpuN/nodeM` (else `node/nodeM/cpulist`), `cpu/cpuN/topology/thread_siblings_list`, `node/has_memory` (else `node/online`) | each CPU's node and physical core; which nodes have memory |
| affinity | the CPU set bessd was started with (cgroup cpuset, Kubernetes CPU Manager, taskset) | the CPUs bessd may use |
| allowed memory | `get_mempolicy(MPOL_F_MEMS_ALLOWED)`, else (refused, as under Docker's or Kubernetes' RuntimeDefault seccomp profile without `CAP_SYS_NICE`) `Mems_allowed_list` in `/proc/self/status` (cgroup `cpuset.mems`, Kubernetes Memory Manager) | the nodes bessd may allocate on |
| flags | `--numa_node` (default -1, automatic), `--control_cpus` (default `auto`) | the operator's choice |

A CPU sysfs does not place on a node is on node 0 (what DPDK assumes too); a
CPU with no sibling list is its own core.

## The rules

**Memory node.** Candidates are the nodes of bessd's CPUs that also have
memory bessd may use.

- `--numa_node=N`: N. If N is not a candidate, a warning, and N anyway.
- One candidate: it, silently. This is every single-node host, every
  Kubernetes pod with Topology Manager `single-numa-node`, every bessd
  restricted to one node's CPUs.
- Several: the lowest, with a warning naming them and suggesting
  `--numa_node` (or a CPU set on one node).
- None (the CPUs are all on nodes without usable memory): the lowest node
  whose memory bessd may use, with a warning. Memory has to come from a node
  the kernel lets bessd allocate on, or nothing can be allocated at all;
  remote CPUs are only slower.

Then, once: DPDK's `--socket-limit` gives every other node a 1 MB limit, which
no hugepage fits under (DPDK reads 0 as "no limit"), and the memory node `-m`
or no limit (DPDK takes 32 nodes, `RTE_MAX_NUMA_NODES`; the list stops
there); `set_mempolicy(MPOL_PREFERRED, node)` makes normal (libc) memory
-- rings, the no-hugepage DPDK heap -- follow (refused by a seccomp profile:
a warning, and only libc memory loses the preference); the one default packet
pool is created there; every table (route, exact match, flow, meters, handoff
channels, DPDK arrays) is allocated there. Under `--no-huge` DPDK labels its
whole heap socket 0, so tables ask DPDK for socket 0 while the pages
physically follow the memory policy.

**Control CPU.** `--control_cpus=auto` reserves one CPU: the first CPU of the
memory node in bessd's set; none when the set has one CPU (control threads
then share it with the worker, warned). gRPC, DPDK's main lcore and the
maintenance loop run there. `--control_cpus=none` keeps D-027's rule
(control threads avoid only the workers' CPUs); `--control_cpus=<list>` names
the CPUs. Only bessd reserves: a test or benchmark that brings DPDK up lazily
keeps every thread on the CPU set it was started with, as under `none`.

**Automatic workers.** A worker given no core (control_v2 `Worker.core`
unset, or `-c -1` for the default worker) takes, from the memory node's CPUs
in the set minus the control CPU minus every CPU a worker holds, the first in
SMT-striped order: the first thread of every physical core (cores in order of
their lowest CPU), then every core's second thread, and so on. The control
CPU holds its core's first thread, so its sibling comes after every free
first thread. Without sibling information the order is lowest first. The CPU
is kept for the worker's life; applying the same desired state again moves
nothing. Nothing free: `RESOURCE_EXHAUSTED` (ENOSPC), an answer clients do
not retry. Never co-located.

A left-out core no longer means CPU 0: a client built from the proto before
D-123 sends nothing for `core: 0`, which is now automatic; explicit core 0
needs a client built from the current proto.

**Explicit cores** are always honoured inside bessd's CPU set: on another node,
on the control CPU, on a CPU another worker holds, on a busy sibling -- each
with a warning, no worker ever moved. Explicitly held CPUs count as used for
later automatic picks.

**The one hard error: outside the CPU set.** A worker's core (`AddWorker`,
`Worker.core`: EINVAL listing the set), `-c` and `--control_cpus` naming a CPU
outside bessd's set are refused. The set is the operator's grant whatever
enforces it: under a cgroup cpuset `sched_setaffinity` would fail, but under
taskset or systemd `CPUAffinity=` alone a thread could widen its own
affinity, so the refusal is bessd's policy, not the kernel's.

## Diagnostics

One INFO line at startup says it all:

```
placement: node 0 (candidates {0}, reason: the only node with both bessd's CPUs and its memory), control CPUs 6, worker CPUs available 7-9
```

followed by a WARNING for each finding. The same line is `GetCapabilities`
`placement` (with `memory_node`), and `bessctl status` prints it.

| finding | when | where it shows |
|---|---|---|
| ambiguous node | several candidates, no `--numa_node` | startup WARNING |
| no candidate | CPUs and usable memory share no node | startup WARNING |
| `--numa_node` not a candidate | | startup WARNING |
| one CPU | no CPU can be reserved | startup WARNING |
| cross-NUMA worker | a worker's CPU is not on the memory node | WARNING at launch ("worker N: ..."); `GetPipelineResponse.workers[].cross_numa`, `numa_node`; legacy `ListWorkers` |
| worker on the control CPU | | WARNING at launch; `on_control_cpu` |
| shared physical core | a worker's CPU is an SMT sibling of another worker's or of the control CPU | WARNING at launch; `shares_core` |
| two workers on one CPU | explicit core | WARNING at launch |
| cross-NUMA NIC | `rte_eth_dev_socket_id` is another node | WARNING once per port; `GetPortStatus` `numa_node`, `cross_numa` |
| NIC node unknown | `rte_eth_dev_socket_id` is -1 (VMs, some virtual devices) | `numa_node` -1, "unknown"; no warning |

### Degraded performance: cross-NUMA

A worker on another node's CPU reads every packet buffer and table over the
socket interconnect; a NIC on another node DMAs packets and descriptors over
it. Throughput drops and latency rises, often by tens of percent, and nothing
fails. The fix is one of: start bessd with `--numa_node=<the NIC's node>` (and
CPUs of that node), restrict its CPU set to one node, or run one bessd per
node, each with that node's NICs.

## Deployment

- **Give bessd whole cores.** Bare metal: put both SMT siblings of a core in
  the set, or neither. Kubernetes: CPU Manager `static` with
  `full-pcpus-only=true`.
- **Size it.** N workers need N+1 CPUs (the control CPU). N workers without
  any sharing a core: 2N+2 CPUs with SMT (N+1 whole cores) -- in Kubernetes a
  Guaranteed pod requesting 2N+2 CPUs with `full-pcpus-only`.
- **Kubernetes, one node:** a Guaranteed pod (integer CPU requests equal to
  limits), kubelet `cpuManagerPolicy: static` (with
  `cpuManagerPolicyOptions: full-pcpus-only: "true"`),
  `memoryManagerPolicy: Static`, `topologyManagerPolicy: single-numa-node`.
  The pod then gets one node's CPUs, memory and (with the SR-IOV device
  plugin) NICs, and bessd chooses that node silently.
- **Without the Memory Manager** the pod may allocate on every node; bessd
  still chooses its CPUs' node (one candidate), silently.
- **A Burstable pod** runs in the shared pool (every CPU and node): bessd
  warns and uses the lowest node. Fine for testing, not for performance.
- **Multi-node hosts:** one bessd per node, each with `--numa_node` (or a CPU
  set and NICs on that node) and its own RPC address.

## SIMD width

How wide a vector path DPDK and BESS's own kernels may take is decided once,
at startup, before the EAL starts (Decision D-126): `--simd_width=auto|256|512`
(default `auto`; `BESSD_SIMD_WIDTH` in the environment).

| `--simd_width` | DPDK on x86 | DPDK on arm64 | BESS kernels |
|---|---|---|---|
| `auto` | nothing passed: DPDK's default, 256 bits whatever the CPU (no AVX-512 path) | nothing passed: DPDK's default, unlimited | DPDK's default within the CPU (x86 with AVX2: 256; arm64: 128) |
| `256` | `--force-max-simd-bitwidth 256` | nothing passed (unlimited), a WARNING | 256 (arm64: 128) |
| `512` | `--force-max-simd-bitwidth 512` | nothing passed (unlimited), a WARNING | 512 (arm64: 128; no BESS kernel has an AVX-512 variant yet) |

On x86, a width the CPU lacks (`512` without AVX-512 F, BW, CD, DQ and VL) is
a WARNING, and bessd proceeds with the CPU's widest (256 with AVX2), which is
also what DPDK is given. On arm64 bessd detects Advanced SIMD (128) but not
SVE, so it cannot know the CPU's widest vectors: clamping DPDK to 128 would
turn off SVE paths DPDK would otherwise take (hns3's need 256), so an explicit
width only applies to BESS's kernels and DPDK keeps its default. Forced, DPDK
holds the width for the life of the process. DPDK limits by it the vector
Rx/Tx paths of most PMDs (the AVX-512 ones: i40e, ice, iavf, idpf, cpfl,
virtio), ACL classify, FIB lookup, `rte_net_crc`, `rte_member`, `rte_thash`,
EFD and the distributor. `rte_lpm` (fixed at DPDK's build) and `rte_hash` (CPU
flags at table creation) ignore it.

One INFO line at startup, also `GetCapabilities` `simd` (with the kernels'
width in bits, `simd_width`) and in `bessctl status`; after the EAL starts a
second line gives DPDK's own limit, also `GetCapabilities` `dpdk_simd_width`
(32768: unlimited):

```
simd: width 256 (requested auto; CPU 256, DPDK default 256)
DPDK SIMD limit: 256
```

A BESS kernel with variants for several widths is a `bess::arch::RuntimeDispatch`
(`core/arch/simd_dispatch.h`): its function pointer is chosen for the width
when bessd starts, never per packet. A program that does not set the width
(tests, benchmarks) runs at `auto`'s width.

## Scenarios

Each row is one test of `core/runtime/numa_placement_test.cc`
(`NumaPlacementTest.*`) unless the test is named in full; "first auto
workers" are the CPUs the first automatic workers get. Two-node rows use CPUs
0-7 on node 0 and 8-15 on node 1, memory on both, unless the row says
otherwise.

| # | scenario | node | control CPU | first auto workers | warnings | test |
|---|---|---|---|---|---|---|
| 1 | bare metal, 1 node, SMT (siblings i, i+10), all 20 CPUs | 0 | 0 | 1, 2 | none | `BareMetalOneNodeSmt` |
| 2 | bare metal, 1 node, SMT off | 0 | 0 | 1, 2 | none | `BareMetalOneNodeNoSmt` |
| 3 | 2 nodes, taskset to node 1's CPUs, mems {0,1} | 1 | 8 | 9, 10 | none | `TwoNodesTasksetToNodeOne` |
| 4 | 2 nodes, all CPUs, all mems | 0 | 0 | 1, 2 | several candidates | `TwoNodesAllCpusWarnsAmbiguity` |
| 5 | 2 nodes, all CPUs, `--numa_node=1` | 1 | 8 | 9, 10 | none | `TwoNodesFlagChoosesNodeOne` |
| 6 | node-0 CPUs, `--numa_node=1` (not a candidate) | 1 | 0 | 1, 2 | not a candidate; cross-NUMA per worker | `FlagOutsideCandidatesWarns` |
| 7 | node ids 0 and 2, taskset to node 2 | 2 | 4 | 5, 6 | none | `NonContiguousNodeIds` |
| 8 | memory-only node (CXL) beside node 0 | 0 | 0 | 1, 2 | none | `MemoryOnlyNodeIgnored` |
| 9 | CPUs only on a CPU-only node 1, mems {0} | 0 | 4 | 5, 6 | no candidate; cross-NUMA per worker | `CpuOnlyNodeUsesAllowedMemoryNode` |
| 10 | container cpuset 4-7 (no CPU 0) | 0 | 4 | 5, 6 | none | `ContainerCpusetWithoutCpuZero` |
| 11 | container, CPUs on both nodes, cpuset.mems {1} | 1 | 8 | 9, 10 | none | `ContainerMemsRestrictToNodeOne` |
| 12 | container, node-0 CPUs, mems {1} | 1 | 0 | 1, 2 | no candidate; cross-NUMA per worker | `ContainerCpusAndMemsDisjoint` |
| 13 | 1-CPU container (CPU 5) | 0 | none | 5, then ENOSPC | one CPU | `OneCpuContainerSharesTheCpu` |
| 15 | no hugepages (or free hugepages on one node only) | as without | | | none | hugepages are no input of the resolver; `ThreadPlacementTest.WithoutHugepagesTablesAllocateOnSocketZero` |
| 16 | k8s Guaranteed, CPU+Memory Manager static, single-numa-node (CPUs 10-13, mems {1}) | 1 | 10 | 11, 12 | none | `K8sGuaranteedSingleNumaNode` |
| 17 | k8s Guaranteed, Memory Manager none (CPUs 10-13, all mems) | 1 | 10 | 11, 12 | none | `K8sGuaranteedWithoutMemoryManager` |
| 18 | k8s Burstable, shared pool (all CPUs and mems) | 0 | 0 | 1, 2 | several candidates | `K8sBurstableSharedPoolWarns` |
| 20 | VM without `/sys/devices/system/node` | 0 | 0 | 1, 2 | none | `VmWithoutNodeDirectory` |
| 21 | VM, node directory without CPU links | 0 | 0 | 1, 2 | none | `VmNodeDirectoryWithoutCpuLinks` |
| 22 | NIC with `rte_eth_dev_socket_id` -1 | | | | none ("unknown") | `DeviceWithUnknownNode` |
| 23 | VM, `thread_siblings_list` = the CPU itself | 0 | 0 | 1, 2 | none | `VmVcpusWithoutSmt` |
| 24 | NIC on the other node | | | | cross-NUMA, once per port | `DeviceOnOtherNodeIsCross` |
| 25 | explicit worker on another node's CPU | | | | cross-NUMA | `ExplicitWorkerOnOtherNodeWarns` |
| 26 | explicit worker on the control CPU | | | | on the control CPU | `ExplicitWorkerOnControlCpuWarns` |
| 27 | explicit worker outside the affinity | | | | refused, EINVAL | `ExplicitWorkerOutsideAffinityIsOutside`; `ThreadPlacementTest.WorkersOutsideTheSetAreRefused` |
| 28 | allocator exhaustion | | | | ENOSPC, `RESOURCE_EXHAUSTED` | `AllocatorExhaustion`; `AutomaticWorkerTest.AnAutomaticWorkerWithNoFreeCpuIsRefusedWithEnospc`; `ApiV2ServiceTest.NoFreeCpuForAnAutomaticWorkerIsResourceExhausted` |
| 29 | stability across re-apply | | | | none | `AllocatorIsStable`; `ApplyPipelineTest.ReapplyingAWorkerWithoutACoreKeepsItsCpuAndReplacesNothing`; `ApiV2ServiceTest.AnAutomaticWorkerIsReportedWhereItRunsAndReapplyIsANoOp` |
| 30 | a destroyed worker's CPU is reused | | | lowest free | none | `FreedCpuIsReused`; `AutomaticWorkerTest.ADestroyedWorkersCpuGoesToTheNextAutomaticWorker` |
| i | N+1 whole cores (8 CPUs, siblings i, i+4), 3 workers | 0 | 0 | 1, 2, 3 (siblings idle) | none | `StripedWholeCoresOnePerCore` |
| ii | N whole cores (6 CPUs, siblings i, i+3), 3 workers | 0 | 0 | 1, 2, 3 | worker 3 shares the control CPU's core | `StripedLastWorkerOnControlSibling` |
| iii | 10 cores / 20 threads, 12 workers | 0 | 0 | 1-9, then 10, 11, 12 | each of 10, 11, 12 shares a core | `StripedFirstThreadsThenSiblings`, `StripedOrderWithAdjacentSiblings` |
| iv | VM without sibling information | 0 | 0 | 1, 2, ... (lowest first) | none | `StripedWithoutSiblingsIsLowestFirst` |
| v | a first-thread worker destroyed | | | that first thread again | none | `StripedReusesFreedFirstThread` |
| vi | explicit core = an automatic worker's CPU | | | the automatic worker unchanged | already runs another worker | `ExplicitCoreOnAnAutoWorkersCpuWarns`; `AutomaticWorkerTest.AnExplicitCoreOnAnAutomaticWorkersCpuIsHonouredBesideIt` |
| vii | explicit worker, then an automatic one | | | skips the explicit CPU | none | `AutoWorkerSkipsExplicitCpu`; `AutomaticWorkerTest.AnAutomaticWorkerSkipsTheCpuOfAnExplicitOne`; `ApplyPipelineTest.AnAutomaticWorkerSkipsACpuAnExplicitWorkerOfThePipelineNames` |
| viii | explicit core on the control CPU | | | | on the control CPU | `ExplicitWorkerOnControlCpuWarns`; `AutomaticWorkerTest.AnExplicitCoreOnTheControlCpuIsHonouredWithAWarning` |
| ix | `--control_cpus` naming a CPU outside the set | | | | startup refused | `ControlCpusOutsideSetRefused` |
| x | taskset-only restriction, worker on a CPU outside it | | | | refused, EINVAL (not widened) | `ThreadPlacementTest.WorkersOutsideTheSetAreRefused` in `control_thread_placement_test_in_two_cpus` (under taskset) |
| b | explicit worker on the control CPU's sibling | | | | shares the control CPU's core | `ExplicitWorkerOnControlSiblingWarns` |
| c | no `thread_siblings_list` at all | 0 | 0 | 1, 2 | none | `MissingSiblingListsNeverWarn` |
| d | a test or benchmark brings DPDK up lazily | | none reserved | | none | `ThreadPlacementTest.LazyEalBringUpKeepsTheInheritedSet` |
| e | seccomp refuses `get_mempolicy` (RuntimeDefault, no `CAP_SYS_NICE`) | as allowed by the cpuset | | | none (`set_mempolicy` refused: a warning) | `MemsAllowedReadsTheStatusFile`, `MemsAllowedFallbackAgreesWithTheKernel` |

Rows 14 and 19 (SMT-aware reservation) were dropped: SMT changes no placement
except the striped order; it is otherwise only diagnosed.
