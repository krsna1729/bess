# Limits

Every fixed limit and default a configuration can reach, in one place. A
change to one of these constants changes this page in the same commit.

Columns:
- **Configurable:** the init argument, command or flag that sets it. "No" means
  a source change is needed.
- **When exceeded:** what the caller sees. EINVAL/ENOSPC are the `error.code`
  of the legacy command, or of the control-v2 error.

## Daemon and runtime

| Limit | Value | Where | Configurable | When exceeded |
|---|---|---|---|---|
| Workers | 64 | `Worker::kMaxWorkers` (`worker.h`), `dataplane::kMaxWorkers` (`dataplane/worker_id.h`), `EventHub::kMaxWorkers` | No | EINVAL "Invalid worker id" |
| Sockets in a placement mask | 16 | `MAX_NUMA_NODE` (`module.h`, builds `UNCONSTRAINED_SOCKET`) | No | not checked; higher sockets cannot be expressed in the mask (no in-tree port sets a mask since D-123: a NIC's node is diagnosed, not a constraint) |
| CPU or node id in a placement CPU list | 65,536 | `kMaxListValue` (`runtime/numa_placement.cc`) | No | the list is malformed: `--control_cpus` refuses start-up; a sysfs list beyond it is ignored (the CPU is then node 0 / its own core) |
| NUMA nodes in the memory policy masks | 1,024 | `MemsAllowed`, `ApplyProcessPlacement` (`runtime/numa_placement.cc`) | No | a higher memory node is not seen as allowed; preferring one is skipped with a warning |
| SIMD width (DPDK paths and BESS kernels, D-126) | `auto`: DPDK's default, 256 bits on x86 (no AVX-512), unlimited on arm64; never above the CPU's | `arch::kDpdkDefaultSimdWidth` (`arch/dpdk_simd.h`, DPDK's `RTE_VECT_DEFAULT_SIMD_BITWIDTH`) | `--simd_width` (`auto`, `256`, `512`) | wider than the CPU: a WARNING, and the CPU's width; any other value refuses start-up |
| Packet buffers | 262,144 | `--buffers` (`runtime/opts.cc`) | `--buffers` | allocation fails at start-up |
| Packet data room | 2,048 B (`SNBUF_DATA`) | `--packet_data_room` | 1 to 65,535 − headroom (`kMaxPacketDataSize`, `packet.h`) | start-up refused |
| Packet metadata area | 128 B | `SNBUF_METADATA` (`snbuf_layout.h`) | No | attributes that do not fit get no offset; their readers fail closed |
| Metadata attributes per module | 16 | `kMaxAttrsPerModule` (`metadata.h`) | No | ENOSPC from `AddMetadataAttr` (module init fails) |
| Metadata attribute size | 32 B | `kMetadataAttrMaxSize` (`metadata.h`) | No | EINVAL from `AddMetadataAttr` (module init fails) |
| Metadata default sentinel | `kMetadataOffsetDefault` (−4) | `metadata.h` | No | a provider-less reader with the default contract reads its declared value; `kMetadataOffsetNoRead` (−2) is the best-effort case, `kMetadataOffsetNoWrite` (−1) a skipped write nothing reads, `kMetadataOffsetInvalidScope` (−5) a skipped write whose reader is also reached by a path with no writer, `kMetadataOffsetNoSpace` (−3) a full metadata area |
| Plugin load passes (module inheritance depth) | 10 | `kInheritanceLimit` (`framework/plugin_loader.h`) | No | plugins still unresolved after 10 `dlopen` passes are logged "Failed to load plugin" and the load reports failure |
| Packet pool default capacity | 65,535 | `PacketPool::kDefaultCapacity` (`packet_pool.h`) | pool creation argument | — |
| Packet pool per-core cache | 512 (pools > 1,024 packets) | `PacketPool::kMaxCacheSize` | No | — |
| Burst size | 32 packets | `PacketBatch::kMaxBurst` (`pktbatch.h`) | No | ABI-wide constant |
| Batches per task run | 256 | `MAX_PBATCH_CNT` (`task.h`) | No | `CHECK` abort (a graph that fans out past it is a bug, D-061) |
| gRPC message size, each way | 64 MiB | `kMaxMessageBytes` (`control/api_v2.h`) | No | RESOURCE_EXHAUSTED; a `GetPipeline` with resources larger than this is refused (`RESOURCE_EXHAUSTED`, `EMSGSIZE`): ask for fewer resources by name (D-129), or read one large table with `ReadResource` (D-170), which pages it instead of building one message. `ReadResource` defaults to 4096 entries per page (capped at 65536) and bounds a page at 3 MiB; an entry above about 64 MiB is still `RESOURCE_EXHAUSTED`, so one entry remains one message |
| Resource page entries (ReadResource) | 4096 default, 65536 maximum | `kMaxPageEntries` (`control/api_v2.cc`), `ReadResourceRequest.max_entries` | Per request | The request's value is clamped to the maximum; a page is also bounded at 3 MiB of serialized message (about 70 MiB of entries was tested), a larger single entry travelling alone |
| Default gRPC port | 10514 | `kDefaultPort` (`runtime/opts.cc`) | `--grpc_url` (or the deprecated `-p`) | — |
| Default bind address | 127.0.0.1 | `kDefaultBindAddr` (`runtime/opts.cc`) | `-b` | — |
| HTTP adapter (JSON API, events, `/metrics`, page; D-125) | off | `FLAGS_http_addr` (`runtime/opts.cc`) | `--http_addr HOST:PORT` (the host is required; only a loopback host keeps it local) | — |
| HTTP request headers / body / concurrent connections / time to send a request | 64 KiB and 64 headers / 64 MiB / 64 / 10 s from accept | `kMaxHeaderBytes`, `kMaxHeaders`, `kMaxBodyBytes`, `kMaxConnections`, `kIoTimeout` (`control/http_server.cc`) | No | 431 (400 past 64 headers) / 413 / 503 / 408 |
| Scheduler quiescent interval | 10 µs | `kDefaultQuiescentIntervalUs` (`scheduler.h`) | `BESS_QUIESCENT_INTERVAL_US` (µs, or `rounds`), read when a worker starts | — |
| RCU retire high water | 4,096 pending retirements | `RcuDomain::kDefaultRetireHighWater` | No | the control thread reclaims, waiting for a grace period if needed; the packet path never waits |

## Graph

| Limit | Value | Where | Configurable | When exceeded |
|---|---|---|---|---|
| Gates per module (each direction) | 8,192 | `MAX_GATES` (`gate.h`, `worker.h`) | No | EINVAL at connect / rule add |
| Tasks per module | 32 | `MAX_TASKS_PER_MODULE` (`module.h`) | No | EINVAL "'taskid' must be between 0 and 31" |
| Port queues per direction | 128 | `MAX_QUEUES_PER_DIR` (`port.h`) | No | EINVAL "Invalid number of queues" |
| Port queue size | 4,096 | `MAX_QUEUE_SIZE` (`port.h`) | No | EINVAL "Invalid queue size" |
| Port queue size default | 1,024 RX and TX | `Port::kDefaultIncQueueSize`, `kDefaultOutQueueSize` | port argument | — |
| Port MTU default | 1,500 | `Port::kDefaultMtu` | port argument | — |

## Control plane

| Limit | Value | Where | Configurable | When exceeded |
|---|---|---|---|---|
| Transaction history kept | 4,096 records | `DataplaneTransactions::kMaxRecords` | No | oldest record dropped |
| Pending reference cascades | 4,096 | `TransactionEngine::kMaxPendingCascades` | No | transaction answered BUSY (retry) |

## Modules

| Module | Limit | Value | Where | Configurable | When exceeded |
|---|---|---|---|---|---|
| ExactMatch | fields; bytes per field | 8; 8 | `ExactMatch::kMaxFields`, `kMaxFieldSize` | No | EINVAL at init |
| ExactMatch | rules | grows by doubling; memory only | `EnsureCapacity` (D-010) | — | ENOSPC only if a table cannot be built |
| WildcardMatch | fields; bytes per field | 8; 8 | `WildcardMatch::kMaxFields`, `kMaxFieldSize` | No | EINVAL at init |
| WildcardMatch | distinct masks in use | 8 (master's `MAX_TUPLES`) | `WildcardMatchArg.max_tuples`, 1–1,024 (`kMaxTuplesLimit`) | yes, at init | ENOSPC "too many distinct masks"; a mask is freed when its last rule goes |
| WildcardMatch | rules | 16M (`RecordArena::kMaxChunks` × 1,024) | No | ENOSPC "rule table is full" |
| WildcardMatch | rule record size | 4,096 B | `RecordArena::kMaxRecordSize` (`classifier/record_arena.h`) | No | the arena's constructor `CHECK`s; a rule record is far smaller |
| GenericEncap | fields; bytes per field | 8; 8 | `MAX_FIELDS`, `MAX_FIELD_SIZE` (`generic_encap.h`) | No | EINVAL "at most 8 fields" |
| Update | fields | 16 | `Update::kMaxFields` | No | EINVAL |
| RandomUpdate | fields | 16 | `kMaxVariable` (`random_update.h`) | No | EINVAL |
| Rewrite / FlowGen | template | 1,536 B | `Rewrite::kMaxTemplateSize`, `MAX_TEMPLATE_SIZE` (`flowgen.h`) | No | EINVAL "template is too big" |
| Replicate | gates | 32 | `Replicate::kMaxGates` | No | EINVAL |
| HashLB | gates | 16,384 | `HashLB::kMaxGates` | No | EINVAL |
| HashLB | custom-tuple fields; bytes per field; packet offset | 8; 8; 0-1,024 | `HashLB::kMaxFields`, `kMaxFieldSize`, `kMaxFieldOffset` | No | EINVAL |
| RandomSplit / RoundRobin | gates | 16,384 | `MAX_SPLIT_GATES`, `MAX_RR_GATES` | No | EINVAL |
| VLANSplit | gates | 4,096 (one per VID) | `kNumOGates` | No | — |
| WorkerSplit | gates | 64 (one per worker) | `kNumOGates` | No | — |
| L2Forward | table size; bucket | 64M (default 1,024); 4 | `kMaxTableSize`, `kDefaultTableSize`, `kMaxBucketSize` (`l2_forward.cc`) | `size`, `bucket` | EINVAL |
| Bridge | dynamic MACs | default 1,024 | `BridgeArg.size` | yes | ENOSPC "FDB full" |
| Bridge | flood group members | 64 | `Fdb::kMaxFloodGroup` | No | refused |
| Bridge | domains; interface ids | 65,535; 65,535 | `MacTable::kMaxDomains`, `kMaxValue` | `max_domains` up to the bound | refused |
| Meter | meters | default 1,024 | `MeterArg.capacity` | yes (up to the id width) | EINVAL |
| Meter | meter ids per packet (levels) | 8 | `kMetadataAttrMaxSize / 4` | `MeterArg.levels` 1–8 | EINVAL |
| Router | address families | destination mode: IPv4, IPv6 or both, at least one; next-hop-id mode: no FIB, both families forwarded | `RouterArg.max_ipv4_routes`, `max_ipv6_routes` (D-183) | destination mode: a family is enabled by setting its route capacity; next-hop-id mode takes no capacity | EINVAL "no address family is enabled" (destination); an explicit 0 is refused; in next-hop-id mode a capacity, a non-default `backend` or `flow_hash`, a nonzero `max_groups`, `max_domains` above 1, `domain_attr`, `punt_miss_gate` or `punt_unresolved_gate` is EINVAL "has no FIB", and route and group writes are refused |
| Router | IPv4 routes; tbl8 groups per domain | 4G − 1; default 256 | `RouterArg.max_ipv4_routes`, `max_ipv4_tbl8s`, `Router::kDefaultIpv4Tbl8s` (`modules/router.h`) | yes | EINVAL "ipv4 capacity exceeds backend limits"; a full table refuses the route write (`kTableFull`) |
| Router | IPv6 routes; tbl8 groups per domain | 1G − 1 (rte_fib6's rule pool is 2 × routes); default 8,192 | `RouterArg.max_ipv6_routes`, `max_ipv6_tbl8s`, `Router::kDefaultIpv6Tbl8s` (`modules/router.h`) | yes | a full table refuses the route write (`kTableFull`); the transaction changes nothing |
| Router | IPv6 FIB entry width; memory per domain | 2 B while the largest value (`max_next_hops + max_groups`) and `max_ipv6_tbl8s` are at most 32,767, else 4 B; at 2 B one instance is 32 MiB + 512 B × `max_ipv6_tbl8s` plus the rib | `kMaxTwoByteValue` (`route/route_table.cc`) | follows from the capacities | — (a domain holds one instance until its first IPv6 change and two after, live and spare; double the bytes at 4 B) |
| Router | next hops | destination mode: 16M − 1; next-hop-id mode: 4G − 2 | `LpmRouteTable::kMaxValue` (the FIB's 24-bit route value); `Router::kMaxNextHopsWithoutFib` (the 32-bit id) | `max_next_hops` up to the bound | EINVAL "router capacity exceeds backend limits" |
| Router | next-hop groups | destination mode: 16M − 1 − `max_next_hops`; next-hop-id mode: none | `LpmRouteTable::kMaxValue` (shares the bound with `max_next_hops`) | `max_groups` up to the bound in destination mode; 0 (the default) registers the empty resource | EINVAL "router capacity exceeds backend limits"; a nonzero value in next-hop-id mode is EINVAL |
| Router | route domains | destination mode: 16M; next-hop-id mode: 1 | `route::kMaxRouteDomains` | `max_domains` up to the bound in destination mode | EINVAL |
| Router | members per next-hop group | 64, active and standby together (D-184) | `NextHopGroup::kMaxMembers` | No | refused |
| Router | next-hop state entries | one per next hop (`max_next_hops`), static or bound | `<router>/next_hop_state` (D-184) | with `max_next_hops` | refused (an unknown next hop is a missing reference) |
| Router | neighbor solicitation frame | 86 B (ARP 42 B) | `kMaxNeighborFrameSize` (`route/l3_packet.h`) | No | a caller's output buffer must fit the family's frame |
| Router | IPv6 ND hop limit | 255 | `kNdHopLimit` (`route/l3_packet.h`) | No | a solicitation or advertisement with another value is not parsed |
| Router | DPDK table instance name | 20 characters (the module name, cut, plus `_r<N>`) | `kMaxInstanceName` (`route/route_table.cc`) | No | never: the name is cut (DPDK's ring and `rte_fib` RIB mempool names, D-132) |
| NAT | flows | default 65,536 | `NATArg.capacity`, `max_capacity` | yes | the table grows up to `max_capacity`; past it a new flow's packet is dropped and `bess.table_full` is raised |
| NAT | idle timeout | 300 s | `kDefaultNatTimeout` (`nat/nat.h`), `BasicNat::kDefaultTimeout` | No | — |
| Conntrack | flows | default 65,536 | `ConntrackArg.capacity` | yes | a new connection's packet leaves on gate 1 (not tracked) and `bess.table_full` is raised |
| Conntrack, NAT, tunnel decaps, Router, Firewall, HashLB (L3/L4) | IPv6 extension headers parsed (hop-by-hop, routing, destination options, fragment, AH) | 8 | `kMaxExtensionHeaders` (`packet_parse.h`) | No | packet treated as malformed: Firewall and HashLB drop it (D-188) |
| DRR | flows | default 4,096; max 16M | `DRR::kDefaultNumFlows`, `kMaxNumFlows` | `num_flows` | EINVAL |
| Queue | size | default 1,024; 4–16,384, a power of 2 | `DEFAULT_QUEUE_SIZE` (`queue.cc`); the bounds are literals in `Queue::SetSize` | `size` | EINVAL "must be in [4, 16384]" / "must be a power of 2" |
| Measure | histogram | 100 ns buckets to 100 ms | `kDefaultNsPerBucket`, `kDefaultMaxNs` | `latency_ns_resolution`, `latency_ns_max` | values above the range are counted in the last bucket |
| BPF | snap length | 65,535 | `SNAPLEN` (`bpf.cc`) | No | — |
| WildcardMatch / ExactMatch | key bytes | 64 (fields × bytes) | `kMaxKeyBytes` | No | follows from the field limits |
| WildcardMatch | default `max_tuples` | 8 | `kDefaultMaxTuples` | `max_tuples` | — |
| GenericEncap | header bytes | 64 (fields × bytes) | `MAX_HEADER_SIZE` (`generic_encap.cc`) | No | follows from the field limits |
| HashLB | default hash mode | L4 | `HashLB::kDefaultMode` | `mode` | — |
| DRR | default quantum; per-flow queue | 1,500 B; 2,048 growing ×2 to 8,192 | `kDefaultQuantum`, `kFlowQueueSize`, `kFlowQueueMax` | `quantum`, `max_flow_queue_size` | — |
| Measure | jitter sample probability | 0.05 | `kDefaultIpDvSampleProb` | `jitter_sample_prob` | — |
| FlowGen | smallest TCP packet | 60 B | `kMinTcpPacketSize` (`flowgen.cc`) | No | — |
| unix_socket port | idle poll interval | 50 µs | `UnixSocketPort::kDefaultMinRxInterval` | port argument `min_rx_interval_ns` | — |
| VXLANEncap | UDP destination port | 4789 | `VXLANEncap::kDefaultDstPort` | `dstport` | — |
| Dump | sampling interval | 1 s | `DEFAULT_INTERVAL_NS` (`dump.cc`) | `interval` | — |
| Router | default route domain | 0 | `kDefaultRouteDomainId` (`route/route_domain.h`) | the route key's `domain` | — |
| Router | neighbor next-hop updates published per tick | 64 | `Router::kMaxNeighborUpdatesPerTick` (`modules/router.h`) | No | the rest stay pending for the next tick (D-183) |
| Router | neighbor interfaces | fixed at Init | `RouterArg.interfaces` | No (recreate the Router) | no runtime change; bindings are the runtime `<router>/neighbors` resource |
| Router | neighbor bindings | default min(1,024, `max_next_hops`) | `RouterArg.max_neighbors` | up to `max_next_hops` | EINVAL above `max_next_hops`; a binding past the bound refuses its transaction |
| Router | on-demand resolution (glean) | none | — | No | a route gateway or connected host resolves only through a bound next-hop id; every neighbor needs a binding (D-183) |
| Router | neighbor input rings | 1,024 replies/advertisements, 256 ARP request copies, 1,024 solicitations | `router.cc` | No | the frame or solicitation is dropped and counted; timers retry |

## Other fixed constants

Not reachable as a configuration error, but each one changes behavior.

| Constant | Value | Where | Meaning |
|---|---|---|---|
| `DEFAULT_PRIORITY` | 0xFFFFFFFF | `traffic_class.h` | priority of a child added to a priority class without one |
| `kMaxWait` | 2^20 cycles | `scheduler.h` | longest back-off for a task that reports no work and blocks |
| `kDefaultIntervalNs` | 1 s | `stats/event_throttle.h` | at most one event per throttle per interval (e.g. `bess.table_full`) |
| `kDefaultPacketDataSize` | 2,048 B | `packet.h` | packet data room when `--packet_data_room` is not given |
| `kMinHeadroom` | 256 slots | `classifier/concurrent_exact.h` | free slots an exact table keeps for deletes in a grace period (D-010); it grows before this runs out |
| `kMaxRules` | 64 | `classifier/small_exact.h` | entries of the small linear exact table (internal callers only) |
| `kMaxNodes` | 256 buckets | `l2/mac_table.h`, `l2/packed_mac_table.h` | cuckoo displacement search width; past it an insert reports full |
| `kMaxRetries` | 64 | `l2/packed_mac_table.h` | reader retries while a writer moves an entry; past it the lookup is a miss (L2Forward: default gate, once) |
| `kMaxBatches` | 256 | `flow/shared_flow_table.h` | retirement batches in flight; past it new retirements share the newest batch's (later) grace period |
| `kMaxTimeout`, `kMaxGranularityShift` | tick range / 4; derived | `dataplane/expiry_wheel.h` | longest expiry a wheel accepts (longer ones are clamped); coarsest tick |
| `kDefaultRingSize` | 256 | `utils/lock_less_queue.h` | default ring of the lock-free queue |
| `kDefaultTarget`, `kDefaultWindow`, `kDefaultSlots` | 5 ms, 100 ms, 4,096 | `utils/codel.h` | CoDel defaults; no module uses CoDel (only `codel_test`) |
| `kMaxPidfileLockTrials` | 5 | `bessd.cc` | attempts to lock the pidfile against a running bessd |
| `MAX_TRACE_DEPTH`, `MAX_TRACE_BUFSIZE` | 32, 4,096 B | `module.cc` | module call tracing, compiled only with `SN_TRACE_MODULES` |
| `kPackedPriorityLow`, `kPackedPriorityHigh` | -32,767, 32,766 | `classifier/concurrent_masked.h` | WildcardMatch priorities in this range are packed into the table value and compared there; outside it the rule's record supplies the rank (same result). 32,767 is outside it: its packed form is the chain tag (D-186) |
| `kMaskedMaxKeyBytes` | 64 B | `classifier/masked_exact.h` | widest masked key; sizes the packet path's stack scratch |

## Internal batch bounds (not reachable from configuration)

These constants size stack scratch on the packet path. Callers pass at most a burst (32), so a
configuration cannot reach them.

| Limit | Value | Where |
|---|---|---|
| Classifier / flow / MAC / meter / route batch | 64 | `kMaxBatch` in `classifier/`, `flow/`, `l2/`, `meter/`, `route/` |
| Packet edit plan | 12 steps, 160 data bytes | `EditPlan::kMaxSteps`, `kMaxData` (`packet_edit_plan.h`) |
| Handoff ring item; capacity | 64 B; 2^30 − 1 | `kMaxItemBytes`, `kMaxCapacity` (`dataplane/handoff.h`) |
| Cuckoo displacement path | 3 | `CuckooMap::kMaxCuckooPath` |
| NAT migration step | 256 slots | `kMaxMigrateSlots` (`nat/nat.h`) |
