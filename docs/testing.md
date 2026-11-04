# Testing

`meson test -C BUILD` runs every suite; README.md lists them. This page is
about what a test may cost.

## Memory

**A test declares the most memory it may use**, in MiB, in
`tests/memory/meson.build` (`test_memory_mb`), and is held to it. A test
with no declaration fails `check_test_memory` (suite `architecture`, and
the CI lanes' `layers` step: `tools/ci_profile.py step_layers`).

### How a test is held to it

`meson test` runs every test through `tools/run_capped.py` (the default
test setup's `exe_wrapper`, root `meson.build`), which:

- runs it in a `systemd-run --user --scope` of its own with `MemoryMax` at
  1.5 times the declaration (at least 32 MiB above it) and no swap, so a
  runaway test is stopped by the kernel in its own cgroup
  (`KILL_FACTOR`, `KILL_MIN_HEADROOM_MB`);
- records its peak: the larger of the scope's `memory.peak`, less the page
  cache still charged to it at the end, and `wait4`'s `ru_maxrss` (the
  largest single process). With no user manager, `ru_maxrss` alone. The
  wrapper's docstring says why each, and why not `RLIMIT_AS`: DPDK reserves
  address space it never touches;
- adds the peak a live test reports for a daemon it started in a cgroup of
  its own (`ReportCgroupPeak`, `tests/live/live_daemon.cc`;
  `live_strict_budget`'s bessd);
- fails the test above its declaration: `declared X MiB, peak Y MiB`.

`run_capped_self_test` (suite `architecture`) proves the mechanism by bytes
with `tests/memory/mem_hog.cc`, which touches N bytes. Run on this host
(2026-11-04, release `-j1`; `run_capped_self_test` output in the meson test log):

| Mode | Touched | Declared | Result | Peak |
|---|---|---|---|---|
| scope | 32 MiB | 64 MiB | passes | 35.6 MiB |
| scope | 64 MiB | 48 MiB | fails above declaration | 67.8 MiB |
| scope | 160 MiB | 48 MiB | kernel kills at 80 MiB limit | 82.6 MiB |
| wait4 only | 32 MiB | 64 MiB | passes | 35.6 MiB |
| wait4 only | 64 MiB | 48 MiB | fails above declaration | 67.8 MiB |
| wait4 only | 160 MiB | 48 MiB | fails above declaration | 163.7 MiB |

CI starts a user manager for the scopes (`.github/workflows/ci.yml`, "User
manager for the test scopes") and sets `BESS_TEST_MEM_REQUIRE_SCOPE=1`, so
the self-test fails there if it cannot make one.

The sanitizer lanes record each peak and fail on none
(`BESS_TEST_MEM_MEASURE=1`, `tools/ci_profile.py step_test`): the
declarations are the uninstrumented builds', and ASan's and TSan's shadow
memory is their own.

### The rules, and what enforces them

`tools/check_test_memory.py` (suite `architecture`; CI `layers`) fails when:

- a test has no declaration;
- a declaration is above 1024 MiB (`CEILING_MB`) without a
  `# above the ceiling:` comment above it saying what a smaller test could
  not prove;
- the scheduled declaration sum at `meson test -j1` or `-j3` exceeds 2048 MiB
  (`SUITE_LIMIT_MB`): the larger of the largest `is_parallel: false` test,
  which meson runs alone, and the N largest parallel tests together;
- this page's table does not list a test with its declaration.

A new test is calibrated with `BESS_TEST_MEM_MEASURE=1 meson test -C BUILD
NAME` (it reports its peak and fails on none): declare at least the peak
times 1.25, rounded up to 8 MiB, using the largest uninstrumented local or CI
compiler/architecture measurement. A kernel-killed run is only a lower bound;
finish its calibration with enough headroom to observe a completed run.

### The suite

Measured on 2026-11-04, GCC x86-64, every test scope and each separately
launched bessd in one fresh systemd slice (`BESS_TEST_MEM_SLICE`), the slice
itself held to `MemoryMax=2048M MemorySwapMax=0`. The slice's `memory.peak`
is the **gross charged-memory peak**, without subtracting page cache:

| Build | Meson jobs | Result | Gross slice peak |
|---|---|---|---|
| -O1 | 1 | 250/250 pass | 1383 MiB |
| -O1 | 3 | 250/250 pass | 882 MiB |
| release | 1 | 314/314 pass | 1620 MiB |
| release | 3 | 314/314 pass | 1379 MiB |

Evidence: the enforced runs' meson logs (`enf-fast-j1`, `enf-fast-j3`,
`enf-rel-j1`, `enf-rel-j3`, with their `testlog.json`), and the parent slice's
`memory.peak`. Each run has exactly one intended OOM kill from the runner
self-test. These are measured outcomes under a 2048 MiB enforcing slice,
not proof that every host or future run will fit that limit.
The [recorded command and outcomes](https://github.com/krsna1729/bess/pull/142#issuecomment-6630142618)
also preserve the per-test maximum and runner self-test results.

The declaration scheduler calculation is **1048 MiB** at both `-j1` and
`-j3` (`check_test_memory.py`, `route_route_test` runs alone). That is not a
gross cgroup ceiling: declarations gate the wrapper's cache-adjusted
working-set diagnostic, whereas the slice also charges retained file cache,
the test runner/wrappers, and per-scope accounting overhead. The diagnostic
subtracts the page cache remaining at exit from an earlier gross peak, so
it is not an exact anonymous-memory peak either. These differing counters
explain why 1048 MiB cannot be compared as an aggregate ceiling with the
observed 1620 MiB; the logs do not attribute that difference byte for byte.
Kernel enforcement uses the higher per-test kill limit, not the declaration.
For a whole-suite hard limit, put all scopes in a capped parent slice as
above; the declaration sum alone cannot enforce it.

The sole declaration above 1024 MiB is `route_route_test` (1048 MiB).
`RouterGroupTest.InvalidGroupsAreRefused` constructs the router exactly at
`LpmRouteTable::kMaxValue - 2` next-hop IDs (the 24-bit value boundary);
a smaller router cannot establish the exact boundary. Its measured diagnostic
peaks are in the table below; `tests/memory/meson.build` holds the exception.

### Tests made smaller

Each measured alone with `BESS_TEST_MEM_MEASURE=1`, before and after:

| Test | Before | After | Meaning retained |
|---|---|---|---|
| live tests' bessd | about 793 MiB | about 215 MiB | 32768 rather than 262144 packet buffers; all live tests pass |
| `utils_dpdk_memory_test` | 986 MiB | 331 MiB | release each fallback growth table's pages before testing the next chunk |
| `live_conformance` | 1099 MiB | 785 MiB | smaller pool and `go test -p=2`, measured with an empty Go build cache |
| `traffic_class_bench` | killed at 3000 MiB | 603 MiB | meson smoke cases; measuring tools retain all cases |
| `classifier_cuckoo_scale_bench` | 2077 MiB | 516 MiB | meson smoke cases; measuring tools retain all cases |
| `route_route_domain_bench` | 1827 MiB | 801 MiB | meson smoke cases; measuring tools retain all cases |
| `flow_bench` | 1325 MiB | 551 MiB | smoke cases on normal pages, avoiding a charged 1 GiB hugepage |
| `modules_table_scale_bench` | 990 MiB | 580 MiB | meson smoke cases; measuring tools retain all cases |
| `modules_exact_match_transaction_bench` | 896 MiB | 475 MiB | meson smoke cases; measuring tools retain all cases |

Evidence: 2026-11-04 calibration logs before (`fast-j1`, `rel-j1`,
`bc-*`) and after (`cal-fast`, `cal-rel`, `re-rel`, `conf-fast`, `conf-rel`);
these are calibration diagnostics, not the enforced-run peaks below.
The full benchmark commands remain in the measuring tools; only the
`core/meson.build` smoke-test arguments change.

### Every test

Declared: local calibration (`BESS_TEST_MEM_MEASURE=1 meson test -j1`,
2026-11-04; page cache then still counted), with at least 25% headroom,
rounded up to 8 MiB, raised where CI measured more (run
[41826189434](https://github.com/krsna1729/bess/actions/runs/41826189434)).
Peak: the larger diagnostic from the local enforced `-j1` and `-j3` runs
for that build, by `run_capped.py`, MiB. `-O1`:
`build/fast` (`-Dbuildtype=plain -Dcpp_args=-O1,-g0 -Dcpu=native`, no
benchmarks); release: `-Dbuildtype=release -Dcpu=x86-64-v3
-Dbuild_benchmarks=true -Dbuild_fuzzers=true`, GCC. `-`: not in that build.
The example projects' tests (`examples/*/meson.build`) declare theirs in
their own `test()`.

| Test | Declared | Peak, -O1 | Peak, release |
|---|---|---|---|
| `route_route_test` | 1048 | 835.2 | 835.6 |
| `route_route_domain_bench` | 1008 | - | 800.6 |
| `live_conformance` | 984 | 229.4 | 230.5 |
| `nat_bench` | 984 | - | 826.0 |
| `flow_decision_cache_bench` | 888 | - | 707.8 |
| `framework_memory_scope_test` | 864 | 683.6 | 687.5 |
| `conntrack_bench` | 816 | - | 652.7 |
| `modules_wildcard_match_update_bench` | 808 | - | 647.5 |
| `traffic_class_bench` | 760 | - | 605.3 |
| `packet_pool_test` | 744 | 592.2 | 592.4 |
| `modules_table_scale_bench` | 728 | - | 580.0 |
| `flow_bench` | 696 | - | 551.1 |
| `route_bench` | 656 | - | 518.9 |
| `classifier_cuckoo_scale_bench` | 648 | - | 516.0 |
| `modules_exact_match_transaction_bench` | 600 | - | 475.1 |
| `utils_memory_backing_bench` | 552 | - | 444.8 |
| `live_strict_budget` | 528 | 420.0 | 422.4 |
| `utils_cuckoo_map_bench` | 528 | - | 419.6 |
| `fib_bench` | 512 | - | 404.4 |
| `modules_exact_match_update_bench` | 512 | - | 406.7 |
| `object_table_bench` | 488 | - | 394.8 |
| `modules_router_test` | 440 | 344.6 | 345.9 |
| `live_bessctl` | 408 | 296.3 | 307.6 |
| `route_router_transaction_test` | 408 | 322.8 | 323.0 |
| `utils_dpdk_memory_test` | 600 | 314.3 | 314.5 |
| `meter_bench` | 368 | - | 292.6 |
| `bessctl_manifest_test` | 352 | 275.2 | 275.3 |
| `control_api_v2_test` | 344 | 271.2 | 272.8 |
| `live_classifier_traffic` | 344 | 243.7 | 245.3 |
| `control_dataplane_transactions_test` | 336 | 265.7 | 267.2 |
| `run_capped_self_test` | 328 | 259.9 | 259.5 |
| `live_control_sdk` | 320 | 221.7 | 223.3 |
| `route_route_domain_test` | 320 | 249.6 | 250.1 |
| `live_dataplane_transactions` | 312 | 217.5 | 218.8 |
| `live_events` | 312 | 217.4 | 218.9 |
| `live_http` | 312 | 218.3 | 219.3 |
| `live_match_modules` | 312 | 217.6 | 219.1 |
| `live_metrics` | 312 | 217.8 | 218.9 |
| `live_module_traffic` | 312 | 217.6 | 218.9 |
| `live_simd_width` | 312 | 217.7 | 219.0 |
| `live_stateful_modules` | 312 | 217.5 | 219.1 |
| `live_usage_report` | 312 | 217.8 | 219.5 |
| `sample_plugin_load` | 312 | 217.6 | 218.9 |
| `expiry_bench` | 288 | - | 224.8 |
| `control_apply_pipeline_test` | 280 | 222.1 | 223.9 |
| `modules_transaction_conformance_test` | 280 | 219.9 | 221.4 |
| `control_admin_v2_test` | 272 | 215.9 | 217.5 |
| `control_scheduling_constraints_test` | 272 | 212.7 | 213.8 |
| `control_thread_placement_test_in_two_cpus` | 272 | 211.4 | 212.1 |
| `control_thread_placement_test` | 272 | 211.3 | 212.2 |
| `framework_exact_match_table_test` | 272 | 216.6 | 217.0 |
| `grace_period_bench` | 272 | - | 212.2 |
| `modules_port_inc_test` | 272 | 211.7 | 212.2 |
| `modules_queue_inc_test` | 272 | 211.4 | 212.7 |
| `modules_session_pipeline_test` | 272 | 215.0 | 216.7 |
| `utils_tcp_flow_reconstruct_test` | 272 | 216.9 | 217.3 |
| `dataplane_object_table_rcu_test` | 264 | 209.1 | 209.4 |
| `modules_acl_test` | 264 | 209.8 | 210.6 |
| `modules_arp_responder_test` | 264 | 209.8 | 210.5 |
| `modules_bpf_test` | 264 | 209.8 | 210.8 |
| `modules_buffer_test` | 264 | 209.8 | 210.9 |
| `modules_bypass_test` | 264 | 209.8 | 210.4 |
| `modules_conntrack_test` | 264 | 209.7 | 210.7 |
| `modules_dump_test` | 264 | 209.8 | 210.8 |
| `modules_ether_encap_test` | 264 | 210.1 | 210.8 |
| `modules_exact_match_test` | 264 | 209.6 | 210.8 |
| `modules_firewall_test` | 264 | 210.2 | 211.0 |
| `modules_flowgen_test` | 264 | 209.7 | 210.8 |
| `modules_generic_decap_test` | 264 | 210.0 | 210.6 |
| `modules_hash_lb_test` | 264 | 209.7 | 211.0 |
| `modules_ip_checksum_test` | 264 | 209.9 | 210.4 |
| `modules_ip_encap_test` | 264 | 209.9 | 210.5 |
| `modules_ipswap_test` | 264 | 209.7 | 210.6 |
| `modules_l4_checksum_test` | 264 | 209.7 | 210.5 |
| `modules_macswap_test` | 264 | 209.8 | 210.7 |
| `modules_measure_test` | 264 | 209.7 | 210.8 |
| `modules_meter_test` | 264 | 209.9 | 210.6 |
| `modules_mpls_pop_test` | 264 | 209.8 | 211.2 |
| `modules_nat_test` | 264 | 209.8 | 210.8 |
| `modules_queue_test` | 264 | 209.8 | 210.6 |
| `modules_random_split_test` | 264 | 209.8 | 210.8 |
| `modules_random_update_test` | 264 | 210.0 | 210.8 |
| `modules_replicate_test` | 264 | 209.9 | 210.8 |
| `modules_rewrite_test` | 264 | 210.1 | 210.8 |
| `modules_round_robin_test` | 264 | 209.7 | 210.8 |
| `modules_set_metadata_test` | 264 | 209.9 | 210.6 |
| `modules_source_test` | 264 | 209.7 | 210.8 |
| `modules_split_test` | 264 | 210.1 | 210.9 |
| `modules_static_nat_test` | 264 | 209.9 | 210.8 |
| `modules_timestamp_test` | 264 | 209.7 | 210.6 |
| `modules_update_test` | 264 | 210.0 | 210.9 |
| `modules_update_ttl_test` | 264 | 209.8 | 210.4 |
| `modules_url_filter_test` | 264 | 209.8 | 210.5 |
| `modules_vlan_pop_test` | 264 | 209.8 | 210.7 |
| `modules_vlan_push_test` | 264 | 209.8 | 210.8 |
| `modules_vlan_split_test` | 264 | 209.8 | 210.8 |
| `modules_vxlan_decap_test` | 264 | 209.6 | 210.8 |
| `modules_vxlan_encap_test` | 264 | 210.0 | 211.0 |
| `modules_wildcard_match_test` | 264 | 210.1 | 210.3 |
| `modules_worker_split_test` | 264 | 210.1 | 210.3 |
| `rcu_rcu_scheduler_test` | 264 | 209.3 | 209.8 |
| `sample_plugin_sequential_update` | 264 | 209.9 | 210.7 |
| `route_forward_test` | 232 | 181.3 | 181.2 |
| `control_api_server_test` | 200 | 154.6 | 155.5 |
| `flow_expiry_bench` | 200 | - | 159.6 |
| `modules_drr_test` | 200 | 155.0 | 156.6 |
| `modules_bridge_test` | 192 | 149.8 | 151.4 |
| `modules_generic_encap_test` | 192 | 149.5 | 150.6 |
| `modules_l2_forward_test` | 192 | 149.5 | 151.0 |
| `modules_packet_store_test` | 192 | 149.6 | 151.0 |
| `modules_vif_test` | 192 | 149.4 | 151.1 |
| `pmd_bench` | 192 | - | 150.3 |
| `drivers_pmd_test` | 184 | 146.0 | 146.3 |
| `drivers_unix_socket_test` | 184 | 145.9 | 146.1 |
| `classifier_range_vs_firewall_bench` | 176 | - | 138.2 |
| `rcu_rcu_worker_test` | 176 | 137.4 | 137.8 |
| `control_control_plane_test` | 168 | 129.1 | 130.0 |
| `handoff_bench` | 168 | - | 128.8 |
| `modules_firewall_path_bench` | 168 | - | 129.8 |
| `modules_firewall_update_bench` | 168 | - | 129.9 |
| `modules_match_path_bench` | 168 | - | 129.7 |
| `modules_queue_out_test` | 168 | 128.2 | 128.8 |
| `modules_router_bench` | 168 | - | 129.6 |
| `modules_shared_instance_test` | 168 | 127.8 | 128.6 |
| `packet_tx_checksum_bench` | 168 | - | 130.1 |
| `stats_bench` | 168 | - | 128.5 |
| `stats_counter_paths_bench` | 168 | - | 128.4 |
| `transaction_bench` | 168 | - | 129.7 |
| `update_scale_bench` | 168 | - | 128.9 |
| `dataplane_packet_store_test` | 160 | 122.7 | 122.5 |
| `dataplane_transaction_lifecycle_test` | 160 | 125.6 | 125.8 |
| `gate_test` | 160 | 124.4 | 124.3 |
| `l2_fdb_bench` | 160 | - | 124.0 |
| `metadata_test` | 160 | 126.4 | 126.8 |
| `module_test` | 160 | 126.3 | 126.5 |
| `modules_exact_match_transaction_test` | 160 | 126.7 | 127.7 |
| `modules_wildcard_match_transaction_test` | 160 | 126.7 | 127.6 |
| `dataplane_handoff_test` | 152 | 116.5 | 116.8 |
| `dataplane_handoff_threads_test` | 152 | 119.9 | 120.0 |
| `flow_decision_cache_test` | 152 | 111.4 | 111.5 |
| `ingress_bench` | 152 | - | 116.8 |
| `meter_meter_resource_test` | 152 | 117.4 | 117.3 |
| `route_connected_routes_test` | 152 | 117.3 | 117.5 |
| `route_l3_test` | 152 | 115.5 | 115.5 |
| `route_lpm_resource_test` | 152 | 116.1 | 116.2 |
| `route_neighbor_resolver_test` | 152 | 116.1 | 116.4 |
| `traffic_class_test` | 152 | 117.1 | 117.5 |
| `classifier_concurrent_masked_test` | 144 | 113.2 | 113.1 |
| `classifier_masked_rule_memory_test` | 144 | 112.8 | 112.9 |
| `classifier_record_arena_test` | 144 | 113.0 | 112.8 |
| `dataplane_object_table_publication_test` | 144 | 112.5 | 113.0 |
| `dataplane_object_table_test` | 144 | 111.6 | 111.8 |
| `dataplane_resource_registry_test` | 144 | 111.5 | 111.7 |
| `dataplane_scope_snapshot_test` | 144 | 111.8 | 112.1 |
| `dataplane_slot_table_test` | 144 | 111.5 | 111.6 |
| `dataplane_transaction_engine_test` | 144 | 113.0 | 113.1 |
| `meter_meter_set_test` | 144 | 111.8 | 111.9 |
| `stats_stats_test` | 144 | 111.6 | 111.6 |
| `utils_dpdk_slab_test` | 144 | 111.6 | 111.5 |
| `classifier_shared_writer_bench` | 120 | - | 91.1 |
| `nat_shared_nat_test` | 120 | 91.7 | 91.6 |
| `mempool_bench` | 112 | - | 85.6 |
| `packet_checksum_bench` | 112 | - | 84.3 |
| `packet_bench` | 104 | - | 79.3 |
| `flow_shared_flow_table_test` | 192 | 75.6 | 75.8 |
| `classifier_bench` | 88 | - | 66.7 |
| `control_maintenance_loop_test` | 88 | 66.8 | 67.0 |
| `occupancy_bench` | 88 | - | 67.5 |
| `packet_edit_plan_bench` | 88 | - | 66.5 |
| `conntrack_shared_conntrack_test` | 144 | 59.1 | 58.8 |
| `drivers_pcap_test` | 80 | 62.6 | 63.1 |
| `classifier_concurrent_exact_test` | 144 | 51.6 | 52.1 |
| `classifier_typed_bench` | 72 | - | 52.8 |
| `control_transaction_fuzz_corpus` | 72 | - | 55.4 |
| `packet_checksum_test` | 72 | 53.4 | 53.6 |
| `packet_cursor_test` | 72 | 53.5 | 53.1 |
| `packet_edit_plan_test` | 72 | 53.4 | 53.1 |
| `packet_mutation_test` | 72 | 53.1 | 53.2 |
| `packet_reshape_test` | 72 | 53.2 | 53.9 |
| `packet_test` | 104 | 53.2 | 53.6 |
| `packet_tx_checksum_test` | 72 | 53.6 | 54.0 |
| `check_link_graph` | 256 | 46.2 | 46.4 |
| `classifier_rte_hash_exact_test` | 64 | 50.6 | 50.8 |
| `dataplane_rebuild_resource_test` | 64 | 49.7 | 49.8 |
| `flow_reference_apps_test` | 144 | 49.3 | 49.4 |
| `meter_meter_test` | 64 | 48.9 | 48.8 |
| `nat_nat_usage_test` | 144 | 49.5 | 49.4 |
| `runtime_malloc_policy_test` | 64 | 49.4 | 49.3 |
| `runtime_simd_width_test` | 64 | 48.3 | 48.8 |
| `utils_bpf_program_test` | 64 | 50.6 | 50.3 |
| `l2_fdb_test` | 56 | 43.7 | 43.7 |
| `modules_exact_match_bench` | 48 | - | 35.8 |
| `bessctl_multicall` | 40 | 29.9 | 31.1 |
| `bessd_test` | 40 | 28.7 | 29.4 |
| `classifier_firewall_table_bench` | 40 | - | 30.2 |
| `classifier_masked_bench` | 40 | - | 30.0 |
| `classifier_range_backend_bench` | 40 | - | 29.4 |
| `control_metrics_exposition_test` | 40 | 29.0 | 29.8 |
| `control_pipeline_resources_test` | 40 | 29.3 | 30.5 |
| `control_pressure_monitor_test` | 40 | 28.2 | 29.5 |
| `control_resource_fill_test` | 40 | 28.6 | 29.3 |
| `member_select_bench` | 40 | - | 29.1 |
| `modules_exact_match_migration_test` | 40 | 27.7 | 28.7 |
| `modules_firewall_key_kernels_test` | 40 | 27.1 | 28.2 |
| `modules_macswap_kernels_test` | 40 | 27.1 | 28.4 |
| `modules_url_filter_bench` | 40 | - | 29.4 |
| `modules_vlan_kernels_test` | 40 | 27.3 | 28.4 |
| `modules_wildcard_match_bench` | 40 | - | 30.1 |
| `rcu_bench` | 40 | - | 29.4 |
| `registry_test` | 40 | 28.4 | 29.5 |
| `ring_bench` | 40 | - | 29.7 |
| `tunnel_bench` | 40 | - | 29.4 |
| `utils_checksum_bench` | 40 | - | 29.4 |
| `utils_copy_bench` | 40 | - | 29.4 |
| `utils_cuckoo_map_test` | 512 | 27.3 | 27.5 |
| `check_arch` | 32 | 22.1 | 22.1 |
| `check_public_proto_closure` | 32 | 20.1 | 20.1 |
| `check_scaling` | 32 | 19.9 | 19.8 |
| `flow_worker_flow_table_test` | 32 | 19.9 | 20.1 |
| `nat_nat_test` | 32 | 24.2 | 24.4 |
| `sdk_cpp_client_test` | 32 | 20.9 | 21.3 |
| `sdk_cpp_commit_model_test` | 32 | 20.0 | 20.4 |
| `arch_arch_test` | 24 | 15.2 | 15.3 |
| `arch_kernels_test` | 24 | 15.3 | 15.3 |
| `arch_signal_context_test` | 24 | 15.2 | 15.2 |
| `arch_simd_dispatch_test` | 24 | 15.2 | 15.4 |
| `check_arch_self_test` | 24 | 18.1 | 18.1 |
| `check_installed_headers_self_test` | 24 | 18.3 | 18.1 |
| `check_layer_includes_self_test` | 24 | 16.5 | 16.5 |
| `check_layer_includes` | 24 | 18.6 | 18.5 |
| `check_limits_self_test` | 24 | 15.9 | 16.0 |
| `check_limits` | 24 | 18.2 | 18.2 |
| `check_link_graph_self_test` | 24 | 17.0 | 17.1 |
| `check_scaling_self_test` | 24 | 16.4 | 16.2 |
| `check_test_memory_self_test` | 24 | 16.5 | 16.4 |
| `check_test_memory` | 24 | 17.1 | 17.5 |
| `checksum_plan_fuzz_corpus` | 24 | - | 15.3 |
| `classifier_backend_test` | 24 | 15.2 | 15.1 |
| `classifier_classifier_api_test` | 24 | 15.2 | 15.2 |
| `classifier_classifier_test` | 24 | 15.1 | 15.2 |
| `classifier_cuckoo_exact_test` | 24 | 15.3 | 15.2 |
| `classifier_direct_exact_test` | 24 | 15.1 | 15.2 |
| `classifier_extract_plan_test` | 24 | 15.2 | 15.2 |
| `classifier_firewall_kernels_test` | 24 | 15.3 | 15.3 |
| `classifier_firewall_table_test` | 24 | 15.2 | 15.3 |
| `classifier_generation_test` | 24 | 15.2 | 15.2 |
| `classifier_masked_exact_test` | 24 | 15.4 | 15.2 |
| `classifier_packed_value_store_test` | 24 | 15.3 | 15.3 |
| `classifier_range_backend_test` | 24 | 15.2 | 15.2 |
| `classifier_result_plan_test` | 24 | 15.2 | 15.2 |
| `classifier_runtime_schema_test` | 24 | 15.3 | 15.3 |
| `classifier_schema_fuzz_corpus` | 24 | - | 15.3 |
| `classifier_small_exact_test` | 24 | 15.2 | 15.2 |
| `classifier_table_schema_test` | 24 | 15.1 | 15.3 |
| `classifier_typed_exact_test` | 24 | 15.2 | 15.3 |
| `cli_mode_test` | 24 | 15.2 | 15.2 |
| `conntrack_conntrack_model_test` | 24 | 15.2 | 15.2 |
| `conntrack_conntrack_test` | 32 | 16.3 | 16.3 |
| `conntrack_fuzz_corpus` | 24 | - | 15.3 |
| `dataplane_batch_stages_test` | 24 | 15.3 | 15.3 |
| `dataplane_continuation_test` | 24 | 15.4 | 15.1 |
| `dataplane_expiry_wheel_test` | 24 | 15.0 | 15.1 |
| `dataplane_identity_test` | 24 | 15.2 | 15.2 |
| `dataplane_member_select_test` | 24 | 15.3 | 15.2 |
| `dataplane_tick_rate_test` | 24 | 15.4 | 15.2 |
| `flow_decision_cache_model_test` | 24 | 15.2 | 15.1 |
| `flow_expiry_consumer_test` | 24 | 15.4 | 15.4 |
| `flow_flow_index_test` | 24 | 15.2 | 15.1 |
| `framework_instance_registry_test` | 24 | 15.2 | 15.2 |
| `framework_module_requests_test` | 24 | 15.2 | 15.2 |
| `framework_module_tick_test` | 24 | 15.3 | 15.0 |
| `framework_plugin_check_test` | 24 | 15.2 | 15.3 |
| `framework_resource_bindings_test` | 24 | 15.2 | 15.3 |
| `framework_threshold_table_test` | 24 | 15.2 | 15.3 |
| `l2_fdb_model_test` | 24 | 15.2 | 15.2 |
| `l2_mac_table_kernels_test` | 24 | 15.3 | 15.1 |
| `l2_mac_table_resource_test` | 24 | 15.0 | 15.3 |
| `l2_packed_mac_table_test` | 24 | 15.2 | 15.1 |
| `nat_fuzz_corpus` | 24 | - | 15.4 |
| `nat_nat_model_test` | 24 | 15.3 | 15.3 |
| `offload_flow_rule_owner_test` | 24 | 15.3 | 15.3 |
| `packet_cursor_fuzz_corpus` | 24 | - | 15.2 |
| `packet_mutation_fuzz_corpus` | 24 | - | 15.3 |
| `packet_parse_test` | 24 | 15.2 | 15.2 |
| `perf_daemon_stop_test` | 24 | 15.3 | 15.2 |
| `port_test` | 24 | 16.4 | 16.2 |
| `rcu_rcu_ptr_test` | 24 | 15.3 | 15.3 |
| `rcu_rcu_test` | 24 | 15.2 | 15.3 |
| `resource_codec_fuzz_corpus` | 24 | - | 15.5 |
| `route_prefix_fuzz_corpus` | 24 | - | 15.1 |
| `runtime_numa_placement_test` | 24 | 15.3 | 15.2 |
| `runtime_startup_test` | 24 | 15.2 | 15.2 |
| `shared_obj_test` | 24 | 15.2 | 15.3 |
| `stats_event_hub_test` | 24 | 15.2 | 15.3 |
| `stats_metric_registry_test` | 24 | 15.4 | 15.2 |
| `tunnel_decap_fuzz_corpus` | 24 | - | 15.4 |
| `tunnel_tunnel_test` | 24 | 15.4 | 15.3 |
| `utils_bits_test` | 24 | 15.2 | 15.2 |
| `utils_checksum_test` | 24 | 15.3 | 15.2 |
| `utils_codel_test` | 24 | 15.2 | 15.2 |
| `utils_copy_test` | 24 | 15.2 | 15.3 |
| `utils_endian_test` | 24 | 15.3 | 15.3 |
| `utils_ether_test` | 24 | 15.3 | 15.3 |
| `utils_extended_priority_queue_test` | 24 | 15.2 | 15.3 |
| `utils_fifo_test` | 24 | 15.4 | 15.3 |
| `utils_histogram_test` | 24 | 15.2 | 15.2 |
| `utils_http_parser_kernels_test` | 24 | 15.3 | 15.3 |
| `utils_inline_function_test` | 24 | 15.3 | 15.2 |
| `utils_ip_test` | 24 | 15.3 | 15.2 |
| `utils_llqueue_test` | 24 | 15.2 | 15.1 |
| `utils_mcslock_test` | 24 | 15.3 | 15.2 |
| `utils_pcap_handle_test` | 24 | 15.4 | 15.3 |
| `utils_time_test` | 24 | 15.5 | 15.5 |
| `utils_trie_test` | 24 | 15.3 | 15.2 |
| `lease_table_test` | 64 | - | - |
| `static_router` | 512 | - | - |
