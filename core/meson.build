core_include = include_directories('.')

rcu_sources = files(
  'rcu/rcu_domain.cc',
)

dataplane_core_sources = files(
  'dataplane/batch_tuning.cc',
  'dataplane/handoff.cc',
  'dataplane/transaction_engine.cc',
)

classifier_sources = files(
  'classifier/concurrent_exact.cc',
  'classifier/concurrent_masked.cc',
  'classifier/extract_plan.cc',
  'classifier/result_plan.cc',
  'classifier/runtime_schema.cc',
)

flow_sources = files(
  'flow/owner.cc',
)

meter_sources = files(
  'meter/meter.cc',
  'meter/meter_set.cc',
)

stats_sources = files(
  'stats/counter_set.cc',
)

route_sources = files(
  'route/route_table.cc',
  'route/router.cc',
)
packet_sources = files(
  'packet.cc',
  'packet_reshape.cc',
  'packet_checksum.cc',
  'packet_tx_checksum.cc',
)

# The EAL layer: DPDK bring-up, memory, process options, thread placement.
# It sits below everything that touches rte_* memory (the classifier, meter,
# route and dataplane libraries bring the EAL up lazily through it) and must
# not reach up into Module, Worker or the registries (tools/check_includes.py
# enforces the includes, tools/check_link_graph.py the archive edges).
eal_sources = files(
  'runtime/thread_placement.cc',
  'runtime/dpdk.cc',
  'runtime/opts.cc',
  'runtime/memory.cc',
  'runtime/path.cc',
  'runtime/startup.cc',
  'utils/dpdk_memory.cc',
  'utils/bpf_program.cc',
)

control_sources = files(
  'control/api_v2.cc',
  'control/control_error.cc',
  'control/control_plane.cc',
  'control/dataplane_transactions.cc',
  'control/pipeline_diff.cc',
  'control/pipeline_plan.cc',
  'control/pipeline_snapshot.cc',
  'control/pipeline_spec.cc',
  'control/pipeline_validator.cc',
  'control/transaction.cc',
)

framework_sources = files(
  'gate.cc',
  'message.cc',
  'metadata.cc',
  'module.cc',
  'module_graph.cc',
  'framework/module_init_context.cc',
  'framework/resource_bindings.cc',
  'framework/plugin_check.cc',
  'framework/plugin_loader.cc',
  'port.cc',
  'shared_obj.cc',
  'stats/worker_histogram.cc',
)

# Workers, tasks, the scheduler and traffic classes, plus the registries they
# publish through. Below bess_framework: nothing here may need a Module symbol
# (a strong one; calls go through Module's virtual interface), which
# tools/check_link_graph.py enforces as the absence of an
# execution -> framework edge.
execution_sources = files(
  'event.cc',
  'framework/instance_registry.cc',
  'packet_pool.cc',
  'resume_hook.cc',
  'runtime/runtime_state.cc',
  'runtime/worker_manager.cc',
  'task.cc',
  'traffic_class.cc',
  'worker.cc',
)

host_sources = files(
  'bessctl.cc',
  'bessd.cc',
  'debug.cc',
)

driver_sources = files(
  'drivers/pmd.cc',
  'drivers/pcap.cc',
  'drivers/unix_socket.cc',
)

module_sources = files(
  'modules/acl.cc',
  'modules/action_table.cc',
  'modules/arp_responder.cc',
  'modules/bpf.cc',
  'modules/bridge.cc',
  'modules/buffer.cc',
  'modules/bypass.cc',
  'modules/drr.cc',
  'modules/dump.cc',
  'modules/ether_encap.cc',
  'modules/exact_match.cc',
  'modules/flowgen.cc',
  'modules/generic_decap.cc',
  'modules/generic_encap.cc',
  'modules/hash_lb.cc',
  'modules/ip_checksum.cc',
  'modules/ip_encap.cc',
  'modules/ip_lookup.cc',
  'modules/ipswap.cc',
  'modules/l2_forward.cc',
  'modules/l4_checksum.cc',
  'modules/macswap.cc',
  'modules/measure.cc',
  'modules/merge.cc',
  'modules/meter.cc',
  'modules/mpls_pop.cc',
  'modules/mttest.cc',
  'modules/nat.cc',
  'modules/noop.cc',
  'modules/packet_store.cc',
  'modules/port_inc.cc',
  'modules/port_out.cc',
  'modules/queue.cc',
  'modules/queue_inc.cc',
  'modules/queue_out.cc',
  'modules/random_split.cc',
  'modules/random_update.cc',
  'modules/replicate.cc',
  'modules/rewrite.cc',
  'modules/round_robin.cc',
  'modules/router.cc',
  'modules/set_metadata.cc',
  'modules/sink.cc',
  'modules/source.cc',
  'modules/split.cc',
  'modules/static_nat.cc',
  'modules/timestamp.cc',
  'modules/update.cc',
  'modules/update_ttl.cc',
  'modules/url_filter.cc',
  'modules/vlan_pop.cc',
  'modules/vlan_push.cc',
  'modules/vlan_split.cc',
  'modules/vif.cc',
  'modules/vxlan_decap.cc',
  'modules/vxlan_encap.cc',
  'modules/wildcard_match.cc',
  'modules/worker_split.cc',
)

utility_sources = files(
  'utils/bpf.cc',
  'utils/copy.cc',
  'utils/endian.cc',
  'utils/ether.cc',
  'utils/fifo_opener.cc',
  'utils/format.cc',
  'utils/http_parser.cc',
  'utils/ip.cc',
  'utils/pcap_handle.cc',
  'utils/simd.cc',
  'utils/stacktrace.cc',
  'utils/syscallthread.cc',
  'utils/time.cc',
)

gate_hook_sources = files(
  'gate_hooks/pcapng.cc',
  'gate_hooks/tcpdump.cc',
  'gate_hooks/track.cc',
)

resume_hook_sources = files(
  'resume_hooks/metadata.cc',
  'resume_hooks/task_graph.cc',
)

low_level_compile_deps = [dpdk_dep, glog_dep]
runtime_compile_deps = low_level_compile_deps + [
  gflags_dep, protobuf_dep, bess_proto_headers_dep,
]
framework_compile_deps = runtime_compile_deps + [pcap_dep]
control_compile_deps = low_level_compile_deps + [
  gflags_dep, protobuf_dep, grpcpp_dep, bess_proto_headers_dep,
] + grpc_reflection_deps
host_compile_deps = common_deps + [bess_proto_headers_dep]
driver_compile_deps = low_level_compile_deps + [
  protobuf_dep, pcap_dep, bess_proto_headers_dep,
]
module_compile_deps = low_level_compile_deps + [
  protobuf_dep, pcap_dep, bess_proto_headers_dep,
]
hook_compile_deps = module_compile_deps
resume_hook_compile_deps = low_level_compile_deps + [
  protobuf_dep, bess_proto_headers_dep,
]
utility_compile_deps = low_level_compile_deps + [pcap_dep]

bess_utils = static_library(
  'bess_utils',
  utility_sources,
  include_directories: core_include,
  dependencies: utility_compile_deps,
)
bess_eal = static_library(
  'bess_eal',
  eal_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps + [pcap_dep],
  link_with: [bess_utils],
)
bess_rcu = static_library(
  'bess_rcu',
  rcu_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
)
bess_dataplane_core = static_library(
  'bess_dataplane_core',
  dataplane_core_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_eal, bess_rcu],
)
bess_classifier = static_library(
  'bess_classifier',
  classifier_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_dataplane_core, bess_eal, bess_rcu],
)
bess_flow = static_library(
  'bess_flow',
  flow_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_classifier, bess_dataplane_core, bess_rcu],
)
bess_meter = static_library(
  'bess_meter',
  meter_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_dataplane_core, bess_eal],
)
bess_stats = static_library(
  'bess_stats',
  stats_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_dataplane_core],
)
bess_route = static_library(
  'bess_route',
  route_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_dataplane_core, bess_eal, bess_rcu],
)
bess_dataplane_libs = [
  bess_dataplane_core,
  bess_classifier,
  bess_meter,
  bess_stats,
  bess_route,
]

bess_packet = static_library(
  'bess_packet',
  packet_sources,
  include_directories: core_include,
  dependencies: low_level_compile_deps,
  link_with: [bess_utils],
)
bess_execution = static_library(
  'bess_execution',
  execution_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
  link_with: bess_dataplane_libs + [bess_eal, bess_packet, bess_rcu, bess_utils],
)
bess_framework = static_library(
  'bess_framework',
  framework_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: framework_compile_deps,
  link_with: bess_dataplane_libs + [bess_execution, bess_eal, bess_packet, bess_rcu, bess_utils],
)
bess_control = static_library(
  'bess_control',
  control_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: control_compile_deps,
  link_with: [bess_framework, bess_execution, bess_eal, bess_dataplane_core, bess_rcu],
)
bess_host = static_library(
  'bess_host',
  host_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: host_compile_deps,
  link_with: [bess_control, bess_framework, bess_execution, bess_eal, bess_dataplane_core, bess_rcu],
)
bess_drivers = static_library(
  'bess_drivers',
  driver_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: driver_compile_deps,
  link_with: [bess_framework, bess_execution, bess_eal],
)
bess_modules = static_library(
  'bess_modules',
  module_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: module_compile_deps,
  link_with: bess_dataplane_libs + [bess_framework, bess_execution, bess_eal, bess_rcu, bess_utils],
  pic: true,
)
bess_gate_hooks = static_library(
  'bess_gate_hooks',
  gate_hook_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: hook_compile_deps,
  link_with: [bess_framework, bess_execution, bess_eal],
)
bess_resume_hooks = static_library(
  'bess_resume_hooks',
  resume_hook_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: resume_hook_compile_deps,
  link_with: [bess_framework, bess_execution, bess_eal],
)

runtime_whole = [
  bess_proto_lib,
  bess_rcu,
  bess_flow,
  bess_packet,
  bess_dataplane_core,
  bess_classifier,
  bess_meter,
  bess_stats,
  bess_route,
  bess_eal,
  bess_execution,
  bess_framework,
  bess_control,
  bess_host,
  bess_utils,
  bess_drivers,
  bess_modules,
  bess_gate_hooks,
  bess_resume_hooks,
]
component_runtime_whole = [
  bess_proto_lib,
  bess_rcu,
  bess_flow,
  bess_dataplane_core,
  bess_classifier,
  bess_meter,
  bess_stats,
  bess_route,
  bess_eal,
  bess_execution,
  bess_framework,
  bess_utils,
  bess_packet,
]

module_graph_runtime_whole = component_runtime_whole + [bess_gate_hooks]
module_runtime_whole = module_graph_runtime_whole + [bess_modules]
driver_runtime_whole = component_runtime_whole + [bess_drivers]
gate_runtime_whole = component_runtime_whole + [bess_gate_hooks]

runtime_deps = common_deps + [bess_proto_dep]
runtime_rpath = dpdk_libdir
bessd_link_args = []
if static_binary_opt != 'none'
  # -static-libgcc and a static libunwind both define the libgcc unwinder entry
  # points (_Unwind_Resume, ...). Naming libgcc_eh in the target's own link
  # arguments, which precede the dependency libraries, makes the compiler
  # runtime win so libunwind's duplicate definitions are never pulled (D-043).
  bessd_link_args += ['-l:libgcc_eh.a']
  # Ubuntu builds its static archives (libunwind.a, libunwind-x86_64.a, ...)
  # as fat LTO objects with the distribution's GCC. The linker plugin would
  # read their LTO bytecode and fail whenever the compiler's major version
  # differs from the archive's ("generated with LTO version 13.1 instead of the
  # expected 14.0"). BESS is not built with LTO; ignore the sections and link
  # the machine code the fat objects also carry (D-055).
  assert(not get_option('b_lto'), 'static builds are linked with -fno-lto; b_lto must be off')
  bessd_link_args += ['-fno-lto']
endif
if static_binary_opt == 'fully_static'
  bessd_link_args += ['-static']
elif static_binary_opt == 'standalone'
  # Distribution libunwind.a is not position independent; link a non-PIE
  # executable (D-043).
  bessd_link_args += ['-static-libgcc', '-static-libstdc++', '-no-pie']
endif

bessd = executable(
  'bessd',
  ['main.cc', version_h],
  include_directories: core_include,
  link_whole: runtime_whole,
  dependencies: runtime_deps,
  export_dynamic: (static_binary_opt != 'fully_static'),
  build_rpath: runtime_rpath,
  link_args: bessd_link_args,
  install: true,
)
core_public_headers = files(
  'commands.h',
  'event.h',
  'gate.h',
  'message.h',
  'metadata.h',
  'module.h',
  'packet.h',
  'packet_checksum.h',
  'packet_cursor.h',
  'packet_handle.h',
  'packet_mutation.h',
  'packet_pool.h',
  'packet_reshape.h',
  'packet_tx_checksum.h',
  'pktbatch.h',
  'port.h',
  'snbuf_layout.h',
  'task.h',
  'worker.h',
)
install_headers(core_public_headers, subdir: 'bess/core')

# Decision D-041 (docs/decisions.md): install a named API set; never recurse
# into core implementation subdirectories.
core_public_subdir_headers = {
  'utils': files(
    'utils/common.h',
    'utils/copy.h',
    'utils/endian.h',
    'utils/ether.h',
    'utils/extended_priority_queue.h',
    'utils/inline_function.h',
    'utils/random.h',
    'utils/time.h',
  ),
  'framework': files(
    'framework/instance_registry.h',
    'framework/module_init_context.h',
    'framework/plugin.h',
  ),
  'dataplane': files(
    'dataplane/action_id.h',
    'dataplane/batch_stages.h',
    'dataplane/batch_tuning.h',
    'dataplane/continuation.h',
    'dataplane/expiry_wheel.h',
    'dataplane/generation_handle.h',
    'dataplane/handoff.h',
    'dataplane/interface_id.h',
    'dataplane/object_table.h',
    'dataplane/resource.h',
    'dataplane/scope.h',
    'dataplane/slot_resource.h',
    'dataplane/slot_table.h',
    'dataplane/strong_id.h',
    'dataplane/tick_rate.h',
    'dataplane/worker_id.h',
  ),
  'classifier': files(
    'classifier/byte_key.h',
    'classifier/classifier.h',
    'classifier/range_backend.h',
  ),
  'flow': files(
    'flow/flow_index.h',
    'flow/flow_key.h',
    'flow/flow_observer.h',
    'flow/flow_storage.h',
    'flow/flow_types.h',
    'flow/owner.h',
    'flow/worker_flow_table.h',
  ),
  'meter': files(
    'meter/meter.h',
    'meter/meter_set.h',
  ),
  'route': files(
    'route/route_table.h',
    'route/next_hop_id.h',
    'route/route_domain.h',
    'route/router.h',
  ),
  'rcu': files(
    'rcu/rcu_domain.h',
    'rcu/rcu_ptr.h',
  ),
  'stats': files(
    'stats/counter_set.h',
    'stats/worker_histogram.h',
    'stats/worker_local.h',
    'stats/worker_slots.h',
  ),
}
foreach subdir, headers : core_public_subdir_headers
  install_headers(headers, subdir: 'bess/core/' + subdir)
endforeach

unit_test_sources = [
  'flow/expiry_consumer_test.cc',
  'flow/flow_index_test.cc',
  'flow/reference_apps_test.cc',
  'flow/worker_flow_table_test.cc',
  'flow/shared_flow_table_test.cc',
  'classifier/classifier_test.cc',
  'classifier/classifier_api_test.cc',
  'classifier/backend_test.cc',
  'classifier/runtime_schema_test.cc',
  'classifier/extract_plan_test.cc',
  'classifier/result_plan_test.cc',
  'classifier/typed_exact_test.cc',
  'classifier/masked_exact_test.cc',
  'classifier/range_backend_test.cc',
  'classifier/generation_test.cc',
  'classifier/packed_value_store_test.cc',
  'classifier/cuckoo_exact_test.cc',
  'classifier/rte_hash_exact_test.cc',
  'classifier/concurrent_exact_test.cc',
  'classifier/concurrent_masked_test.cc',
  'classifier/small_exact_test.cc',
  'classifier/direct_exact_test.cc',
  'dataplane/batch_stages_test.cc',
  'dataplane/continuation_test.cc',
  'dataplane/expiry_wheel_test.cc',
  'dataplane/handoff_test.cc',
  'dataplane/handoff_threads_test.cc',
  'dataplane/tick_rate_test.cc',
  'dataplane/identity_test.cc',
  'dataplane/object_table_test.cc',
  'dataplane/object_table_rcu_test.cc',
  'dataplane/object_table_publication_test.cc',
  'dataplane/transaction_engine_test.cc',
  'dataplane/transaction_lifecycle_test.cc',
  'dataplane/scope_snapshot_test.cc',
  'meter/meter_test.cc',
  'meter/meter_set_test.cc',
  'stats/stats_test.cc',
  'route/route_test.cc',
  'route/router_transaction_test.cc',
  'route/route_domain_test.cc',
  'rcu/rcu_test.cc',
  'rcu/rcu_ptr_test.cc',
  'rcu/rcu_worker_test.cc',
  'rcu/rcu_scheduler_test.cc',
  'control/api_v2_test.cc',
  'control/dataplane_transactions_test.cc',
  'control/thread_placement_test.cc',
  'runtime/startup_test.cc',
  'framework/instance_registry_test.cc',
  'modules/shared_instance_test.cc',
  'control/apply_pipeline_test.cc',
  'control/control_plane_test.cc',
  'bessd_test.cc',
  'gate_test.cc',
  'runtime/memory_test.cc',
  'metadata_test.cc',
  'module_test.cc',
  'modules/bridge_test.cc',
  'modules/drr_test.cc',
  'modules/exact_match_migration_test.cc',
  'modules/exact_match_test.cc',
  'modules/exact_match_transaction_test.cc',
  'modules/l2_table_test.cc',
  'modules/queue_out_test.cc',
  'modules/session_pipeline_test.cc',
  'modules/wildcard_match_test.cc',
  'modules/wildcard_match_transaction_test.cc',
  'modules/vif_test.cc',
  'modules/packet_store_test.cc',
  'packet_cursor_test.cc',
  'packet_test.cc',
  'packet_checksum_test.cc',
  'packet_tx_checksum_test.cc',
  'packet_mutation_test.cc',
  'packet_reshape_test.cc',
  'port_test.cc',
  'shared_obj_test.cc',
  'registry_test.cc',
  'traffic_class_test.cc',
  'drivers/pcap_test.cc',
  'drivers/pmd_test.cc',
  'utils/bits_test.cc',
  'utils/inline_function_test.cc',
  'utils/bpf_program_test.cc',
  'utils/checksum_test.cc',
  'utils/codel_test.cc',
  'utils/cuckoo_map_test.cc',
  'utils/endian_test.cc',
  'utils/ether_test.cc',
  'framework/exact_match_table_test.cc',
  'framework/resource_bindings_test.cc',
  'framework/plugin_check_test.cc',
  'utils/extended_priority_queue_test.cc',
  'utils/fifo_test.cc',
  'utils/histogram_test.cc',
  'utils/ip_test.cc',
  'utils/llqueue_test.cc',
  'utils/pcap_handle_test.cc',
  'utils/tcp_flow_reconstruct_test.cc',
  'utils/time_test.cc',
  'utils/trie_test.cc',
]

taskset_prog = find_program('taskset', required: false)
foreach source : unit_test_sources
  test_name = source.replace('.cc', '').replace('/', '_')
  integration_test = (
    source.startswith('control/') or
    source == 'bessd_test.cc' or
    source == 'registry_test.cc'
  )
  # Tests that only use library code link the libraries normally, so the
  # linker pulls just the objects they reference. A test that needs static
  # registration (ADD_MODULE and the driver, hook and port builders) links the
  # relevant libraries whole, or those constructors would be dropped.
  test_link_with = component_runtime_whole
  test_link_whole = []
  if source.startswith('modules/')
    test_link_whole = module_runtime_whole
    test_link_with = []
  elif source in [
    'dataplane/object_table_rcu_test.cc',
    'metadata_test.cc',
    'module_test.cc',
    'rcu/rcu_scheduler_test.cc',
  ]
    test_link_whole = module_graph_runtime_whole
    test_link_with = []
  elif source.startswith('drivers/')
    test_link_whole = driver_runtime_whole
    test_link_with = []
  elif source == 'gate_test.cc'
    test_link_whole = gate_runtime_whole
    test_link_with = []
  elif integration_test
    test_link_whole = runtime_whole
    test_link_with = []
  endif
  # The test main is ours (core/gtest_main.cc) rather than the packaged one:
  # it configures the death-test style and keeps expected-death children from
  # writing core dumps.
  unit_exe = executable(
    test_name,
    source,
    'gtest_main.cc',
    include_directories: core_include,
    link_with: test_link_with,
    link_whole: test_link_whole,
    dependencies: runtime_deps + [gtest_dep],
    build_rpath: runtime_rpath,
  )
  test_workdir = meson.current_build_dir()
  if source == 'utils/tcp_flow_reconstruct_test.cc'
    test_workdir = meson.current_source_dir()
  endif
  test(
    test_name,
    unit_exe,
    is_parallel: false,
    workdir: test_workdir,
    env: {'LD_LIBRARY_PATH': dpdk_libdir},
  )
  # Placement as a container sees it: bessd started inside a two-CPU set
  # (D-027).
  if source == 'control/thread_placement_test.cc' and taskset_prog.found()
    test(
      test_name + '_in_two_cpus',
      taskset_prog,
      args: ['-c', '0-1', unit_exe.full_path()],
      depends: unit_exe,
      is_parallel: false,
      workdir: test_workdir,
      env: {'LD_LIBRARY_PATH': dpdk_libdir},
    )
  endif
endforeach
test(
  'check_layer_includes',
  python,
  args: [files('../tools/check_includes.py'), '--root', meson.project_source_root()],
  is_parallel: false,
  suite: 'architecture',
)
test(
  'check_link_graph_self_test',
  python,
  args: [files('../tools/check_link_graph.py'), '--self-test'],
  is_parallel: false,
  suite: 'architecture',
)
# The real check reads the built archives; the dependency list makes
# `meson test` build them first.
test(
  'check_link_graph',
  python,
  args: [files('../tools/check_link_graph.py'), '--build-dir',
         meson.project_build_root()],
  depends: [bess_rcu, bess_dataplane_core, bess_classifier, bess_flow, bess_meter,
            bess_stats, bess_route, bess_utils, bess_packet, bess_eal, bess_execution,
            bess_framework, bess_control, bess_host, bess_drivers,
            bess_modules, bess_gate_hooks, bess_resume_hooks, bess_proto_lib],
  is_parallel: false,
  suite: 'architecture',
)

if get_option('build_benchmarks')
  benchmark_sources = {
    'fib_bench': ['fib_bench.cc', benchmark_dep],
    'mempool_bench': ['mempool_bench.cc', benchmark_dep],
    'packet_bench': ['packet_bench.cc', benchmark_dep],
    'packet_checksum_bench': ['packet_checksum_bench.cc', benchmark_main_dep],
    'packet_tx_checksum_bench': ['packet_tx_checksum_bench.cc', benchmark_main_dep],
    'pmd_bench': ['pmd_bench.cc', benchmark_dep],
    'ring_bench': ['ring_bench.cc', benchmark_dep],
    'rcu_bench': ['rcu/rcu_bench.cc', benchmark_main_dep],
    'classifier_bench': ['classifier/classifier_bench.cc', benchmark_main_dep],
    'classifier_typed_bench': ['classifier/typed_exact_bench.cc', benchmark_main_dep],
    'classifier_masked_bench': ['classifier/masked_bench.cc', benchmark_main_dep],
    'classifier_cuckoo_scale_bench': ['classifier/cuckoo_scale_bench.cc', benchmark_main_dep],
    'classifier_shared_writer_bench': ['classifier/shared_writer_bench.cc', benchmark_main_dep],
    'flow_bench': ['flow/flow_bench.cc', benchmark_main_dep],
    'expiry_bench': ['dataplane/expiry_bench.cc', benchmark_main_dep],
    'handoff_bench': ['dataplane/handoff_bench.cc', benchmark_main_dep],
    'flow_expiry_bench': ['flow/flow_expiry_bench.cc', benchmark_main_dep],
    'classifier_range_backend_bench': ['classifier/range_backend_bench.cc', benchmark_main_dep],
    'utils_memory_backing_bench': ['utils/memory_backing_bench.cc', benchmark_main_dep],
    'object_table_bench': ['dataplane/object_table_bench.cc', benchmark_main_dep],
    'transaction_bench': ['dataplane/transaction_bench.cc', benchmark_main_dep],
    'update_scale_bench': ['dataplane/update_scale_bench.cc', benchmark_dep],
    'ingress_bench': ['dataplane/ingress_bench.cc', benchmark_dep],
    'occupancy_bench': ['classifier/occupancy_bench.cc', benchmark_dep],
    'grace_period_bench': ['rcu/grace_period_bench.cc', benchmark_dep],
    'meter_bench': ['meter/meter_bench.cc', benchmark_main_dep],
    'stats_bench': ['stats/stats_bench.cc', benchmark_main_dep],
    'route_bench': ['route/route_bench.cc', benchmark_main_dep],
    'route_route_domain_bench': ['route/route_domain_bench.cc', benchmark_main_dep],
    'traffic_class_bench': ['traffic_class_bench.cc', benchmark_main_dep],
    'modules_url_filter_bench': ['modules/url_filter_bench.cc', benchmark_main_dep],
    'modules_exact_match_bench': ['modules/exact_match_bench.cc', benchmark_main_dep],
    'modules_exact_match_update_bench': ['modules/exact_match_update_bench.cc', benchmark_main_dep],
    'modules_exact_match_transaction_bench': ['modules/exact_match_transaction_bench.cc', benchmark_main_dep],
    'modules_wildcard_match_bench': ['modules/wildcard_match_bench.cc', benchmark_main_dep],
    'modules_wildcard_match_update_bench': ['modules/wildcard_match_update_bench.cc', benchmark_main_dep],
    'modules_table_scale_bench': ['modules/table_scale_bench.cc', benchmark_main_dep],
    'utils_checksum_bench': ['utils/checksum_bench.cc', benchmark_main_dep],
    'utils_copy_bench': ['utils/copy_bench.cc', benchmark_main_dep],
    'utils_cuckoo_map_bench': ['utils/cuckoo_map_bench.cc', benchmark_main_dep],
  }
  benchmark_targets = []
  foreach benchmark_name, benchmark_spec : benchmark_sources
    benchmark_exe = executable(
      benchmark_name,
      benchmark_spec[0],
      include_directories: core_include,
      link_whole: runtime_whole,
      dependencies: runtime_deps + [benchmark_spec[1]],
      build_rpath: runtime_rpath,
    )
    benchmark_targets += benchmark_exe
    test(
      benchmark_name,
      benchmark_exe,
      args: ['--benchmark_min_time=0.001s'],
      is_parallel: false,
      suite: 'benchmarks',
      timeout: 1800,
      env: {'LD_LIBRARY_PATH': dpdk_libdir},
    )
  endforeach
  run_target(
    'benchmarks',
    command: [find_program('true')],
    depends: benchmark_targets,
  )
endif
