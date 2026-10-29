# Running bessd in containers

For containers, run `bessd -f` (or set `BESSD_F=true`) so bessd remains PID 1.
Without it, the legacy daemon mode is still the default and bessd forks.
Other startup flags can come from `BESSD_<FLAG>` (Decisions D-027, D-029,
D-030, D-123). What it reads, and what a container or Kubernetes pod should
give it ([placement.md](placement.md) has the CPU and NUMA rules, every
scenario and the Kubernetes settings):

| bessd needs | where it comes from | override |
|---|---|---|
| CPUs | its inherited CPU set (cgroup cpuset, Kubernetes CPU manager, taskset); one CPU of the memory node is reserved for control threads; a worker given no core takes a free CPU of the memory node, one per physical core first | `-c`, `--control_cpus`, a worker's `core`; anything outside the set is refused |
| memory node | the one NUMA node with both bessd's CPUs and memory it may use (cpuset mems); one bessd drives one node | `--numa_node`, `BESSD_NUMA_NODE` |
| memory (D-198) | hugepages on the memory node if any are usable, else normal pages with a warning; the limit normal pages are admitted against from the cgroup's `memory.max`, else the host's `MemTotal`; tables refused, not OOM-killed, when either runs out | the five `--mem_*` flags, `BESSD_MEM_*`: see [Memory](#memory) |
| packet buffers | `--buffers` (default 262,144 in one pool on the memory node, about 600 MB); size it for your pipeline -- modules need memory too. Admitted like a table (see [Memory](#memory)) | `--buffers`, `BESSD_BUFFERS` |
| NICs | the PCI addresses a device plugin assigned (`PCIDEVICE_*` environment, e.g. the SR-IOV network device plugin); otherwise every device DPDK can use | `-pci_allow`, `BESSD_PCI_ALLOW` |
| IOVA mode | DPDK's choice: VA with an IOMMU (vfio-pci, the recommended binding), PA where hardware needs it | `-iova`, `BESSD_IOVA` |
| HTTP (JSON API, server-sent events, Prometheus `/metrics`, a status page; D-125) | off | `--http_addr`, `BESSD_HTTP_ADDR`: `127.0.0.1:PORT` for the pod alone (a sidecar scraper); `0.0.0.0:PORT` serves the network with no authentication, so only behind a network policy. A request's `Host` must be `localhost`, an IP address (a scrape by pod IP is one) or a DNS name in `--http_allowed_hosts` / `BESSD_HTTP_ALLOWED_HOSTS` (e.g. a Service name) |
| any flag | `BESSD_<FLAG>` environment variable (e.g. `BESSD_GRPC_URL=0.0.0.0:10514`); the command line wins | -- |

Behaviour a container runtime relies on:

- **Foreground (`-f` or `BESSD_F=true`) is container mode:** logs go to stderr,
  and no pidfile or single-instance lock is used unless `-i` names one (a
  container's PID 1 is the only instance, and `/var/run` may be read-only).

- **Background daemon instance locks:** with default `-i`, the default
  `127.0.0.1:10514` listener keeps `/var/run/bessd.pid`; each other effective
  RPC listen address gets a stable hash-suffixed pidfile. `-k` restarts only
  the process holding the selected pidfile. An explicit `-i` is used verbatim;
  choose a distinct path for each independently managed instance. `bessctl
  daemon start` checks the same endpoint-derived path and uses the existing
  warning/confirmation path if that endpoint is already locked.
- This only separates BESS instance identity. Concurrent instances need
  distinct RPC listen addresses and, for DPDK-backed ports, disjoint device
  ownership. Memory and hugepage capacity remain shared host resources.
- **SIGTERM and SIGINT shut down gracefully:** the gRPC server stops, the
  dataplane is torn down in order (workers paused, modules, ports, workers
  destroyed), and bessd exits 0 -- so a pod deletion or `docker stop` is
  clean.
- **Health:** the standard `grpc.health.v1.Health` service answers SERVING
  (usable as a gRPC liveness/readiness probe); server reflection lets
  generic tools list the API without BESS's protos.
- **No root required:** bessd needs its VFIO devices (`/dev/vfio`) and
  hugepages (the hugetlb group or `CAP_IPC_LOCK`); it warns when not root
  and DPDK reports whatever is missing.
- **Nothing left on disk:** DPDK runs in-memory (no hugetlbfs files, no
  runtime directory).
- **Memory limits refuse, they do not kill (D-198):** bessd reads its cgroup's
  `memory.max` at startup and reports it, its source, what is used and what
  is left in `GetResourceStatus.memory` (the startup log has the limit too).
  Usage against a cgroup limit is `memory.current` less reclaimable page cache
  (`memory.stat` `inactive_file`), what the OOM killer weighs. Outside a
  cgroup with a limit -- bare metal, a container without one, a host shared
  with other processes -- the default is the whole host's `MemTotal`, which
  does not protect anything else on the host: give bessd its share with
  `--mem_limit_mb`. The two kinds of memory are accounted apart: hugepages are
  charged to the cgroup's hugetlb controller (`hugetlb.<size>.max`, which
  bounds what bessd maps, see [Memory](#memory)) and, unless cgroup2 is
  mounted with `memory_hugetlb_accounting`, not to `memory.max`. So a pod's
  memory limit has to cover bessd's normal pages -- glibc, gRPC, the
  normal-backing heap and any fallback heap -- plus the reserve, and its
  hugepage limit the hugepage heap. Where cgroup2 has
  `memory_hugetlb_accounting` (Linux 6.6 and later, an opt-in mount option;
  Kubernetes assumes it is off), a hugepage is also charged to `memory.max`
  when it is mapped, and a charge past it is SIGBUS even with free pages in
  the pool: bessd detects the option at startup (`/proc/self/mountinfo`),
  admits hugepage growth against the memory limit too, refuses any heap
  growth that would cross it (a DPDK allocation validator), and warns when
  the limit cannot hold the hugepage cap plus the reserve -- on such a node a
  pod's memory limit must include its hugepages. A refused creation is
  non-retryable (`RESOURCE_EXHAUSTED`, errno `ENOMEM`) and leaves nothing
  behind; free memory (delete tables or sessions), raise the limit, add
  hugepages, or allow the fallback (`--mem_fallback`).

## Memory

What DPDK memory is, how much of it bessd may take, and what happens when it
runs out (D-029, D-030, D-123, D-198) are five flags, each also a
`BESSD_<FLAG>` variable:

| flag | environment | default | what it sets |
|---|---|---|---|
| `--mem_backing` | `BESSD_MEM_BACKING` | `auto` | `auto`: hugepages if any are usable at startup -- host-free pages on the memory node bounded by the remaining cgroup v2 hugetlb capacity (`max - current` along this cgroup's ancestors) -- mapped as needed; none usable: normal pages, with a WARNING. `hugepages`: bessd refuses to start without usable hugepages. `normal`: normal (4 KiB) pages only (DPDK `--no-huge`), whatever the host has |
| `--mem_hugepage_mb` | `BESSD_MEM_HUGEPAGE_MB` | `0`: no cap beyond the cgroup's `hugetlb.<size>.max` | a cap in MB on the hugepage heap on the memory node (DPDK `--socket-limit`; every other node gets none, D-123). With hugepage backing, the cgroup's hugetlb limit -- the smallest finite `hugetlb.<size>.max` along this cgroup's ancestors, summed over the usable page sizes when every one is limited -- caps the heap as well, the smaller of the two winning, so DPDK refuses at the limit instead of faulting a page past it. A table past the cap is refused, or comes from normal pages with `--mem_fallback`. Refused at startup with `--mem_backing=normal` |
| `--mem_fallback` | `BESSD_MEM_FALLBACK` | off | when hugepages run out or reach the cap, a table comes from `bess-normal`, a heap of normal pages on the same node (slower lookups, admitted against the limit), instead of being refused with `RESOURCE_EXHAUSTED`. With normal backing there is nothing to fall back from |
| `--mem_limit_mb` | `BESSD_MEM_LIMIT_MB` | `0`: the lowest cgroup v2 `memory.max` along this cgroup's ancestors, else the host's `MemTotal` | `-1`: no limit; `N`: N MB of bessd's resident set. Normal-page memory (the normal-backing heap, `bess-normal`, DPDK library tables on normal pages, the packet pool) is admitted against it less the reserve; a creation or transaction that would cross it is refused with `RESOURCE_EXHAUSTED` rather than bessd being OOM-killed. With normal backing it also sizes DPDK's heap: the limit less the reserve (512 MB with no limit) |
| `--mem_reserve_mb` | `BESSD_MEM_RESERVE_MB` | `-1`: max(128 MB, limit/16), at most half the limit | MB of the limit kept for memory bessd does not account (glibc, gRPC, thread stacks) |

bessd resolves them once, before DPDK starts, and logs one line naming every
setting and where the limit came from, e.g. `Memory: backing hugepages
(--mem_backing=auto; 2048 MB of hugepages usable, mapped as needed), hugepage
cap 2048 MB (cgroup hugetlb.1GB.max (/kubepods/pod1)), hugetlb not charged to
memory.max, fallback off, limit 6144 MB (cgroup memory.max (/kubepods/pod1)),
reserve 384 MB`. A combination that cannot hold (`hugepages` with none
usable, a cap with `normal`, a reserve that leaves nothing of the limit) stops
bessd with the reason. The packet pool (`--buffers`, `--packet_data_room`) is
admitted like a table -- against the limit with normal pages, the cap with
hugepages -- and a pool that does not fit stops bessd naming both flags.
`GetResourceStatus.memory` reports the resolved policy, both heaps, the
refusals and the bytes each module and resource holds (the packet pool as
`bessd/packet_pool`).

A process that brings DPDK up without bessd -- unit tests, benchmarks, a
library user (`InitDpdk()`) -- reads the same `BESSD_MEM_*` variables, with
the same checks; with none set it gets normal pages and no limit (a 512 MB
heap). `BESSD_MEM_BACKING=hugepages` (with `BESSD_MEM_HUGEPAGE_MB` for a cap)
opts it into hugepages, IOVA-as-VA unless `--iova` says otherwise; a bigger
normal-page heap is `BESSD_MEM_LIMIT_MB` (with `BESSD_MEM_RESERVE_MB=0`, the
heap is exactly the limit).
