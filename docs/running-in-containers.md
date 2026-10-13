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
| memory | host-free hugepages on the memory node bounded by remaining cgroup v2 hugetlb capacity (`max - current` along this cgroup's ancestors), mapped as needed with no cap of bessd's own; none on other nodes; otherwise normal pages with a warning | `-m N` caps at N MB; `-m 0` forces normal pages |
| packet buffers | `-buffers` (default 262,144 in one pool on the memory node, about 600 MB); size it for your pipeline -- modules need memory too | `-buffers`, `BESSD_BUFFERS` |
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
