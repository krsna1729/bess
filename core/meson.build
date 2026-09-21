core_include = include_directories('.', '..')

core_sources = files(
  'bessctl.cc',
  'bessd.cc',
  'control/control_error.cc',
  'control/control_plane.cc',
  'control/runtime_state.cc',
  'debug.cc',
  'dpdk.cc',
  'event.cc',
  'gate.cc',
  'memory.cc',
  'message.cc',
  'metadata.cc',
  'module.cc',
  'module_graph.cc',
  'opts.cc',
  'packet.cc',
  'packet_pool.cc',
  'port.cc',
  'resume_hook.cc',
  'shared_obj.cc',
  'task.cc',
  'traffic_class.cc',
  'worker.cc',
)

driver_sources = files(
  'drivers/pmd.cc',
  'drivers/pcap.cc',
  'drivers/unix_socket.cc',
)

module_sources = files(
  'modules/acl.cc',
  'modules/arp_responder.cc',
  'modules/bpf.cc',
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
  'modules/mpls_pop.cc',
  'modules/mttest.cc',
  'modules/nat.cc',
  'modules/noop.cc',
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

runtime_compile_deps = common_deps + [bess_proto_dep]

bess_core = static_library(
  'bess_core',
  core_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
)
bess_drivers = static_library(
  'bess_drivers',
  driver_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
)
bess_utils = static_library(
  'bess_utils',
  utility_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
)
bess_modules = static_library(
  'bess_modules',
  module_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
  pic: true,
)
bess_gate_hooks = static_library(
  'bess_gate_hooks',
  gate_hook_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
)
bess_resume_hooks = static_library(
  'bess_resume_hooks',
  resume_hook_sources + [proto_ready_marker],
  include_directories: core_include,
  dependencies: runtime_compile_deps,
)

runtime_whole = [
  bess_proto_lib,
  bess_core,
  bess_utils,
  bess_drivers,
  bess_modules,
  bess_gate_hooks,
  bess_resume_hooks,
]
runtime_deps = common_deps + [bess_proto_dep]
runtime_rpath = dpdk_libdir

bessd = executable(
  'bessd',
  ['main.cc', version_h],
  include_directories: core_include,
  link_whole: runtime_whole,
  dependencies: runtime_deps,
  export_dynamic: true,
  build_rpath: runtime_rpath,
  install: true,
)

unit_test_sources = [
  'bessd_test.cc',
  'gate_test.cc',
  'memory_test.cc',
  'metadata_test.cc',
  'module_test.cc',
  'packet_test.cc',
  'port_test.cc',
  'shared_obj_test.cc',
  'registry_test.cc',
  'traffic_class_test.cc',
  'drivers/pcap_test.cc',
  'drivers/pmd_test.cc',
  'utils/bits_test.cc',
  'utils/checksum_test.cc',
  'utils/codel_test.cc',
  'utils/cuckoo_map_test.cc',
  'utils/endian_test.cc',
  'utils/ether_test.cc',
  'utils/exact_match_table_test.cc',
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

foreach source : unit_test_sources
  test_name = source.replace('.cc', '').replace('/', '_')
  unit_exe = executable(
    test_name,
    source,
    include_directories: core_include,
    link_whole: runtime_whole,
    dependencies: runtime_deps + [gtest_main_dep],
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
endforeach

if get_option('build_benchmarks')
  benchmark_sources = {
    'fib_bench': ['fib_bench.cc', benchmark_dep],
    'mempool_bench': ['mempool_bench.cc', benchmark_dep],
    'packet_bench': ['packet_bench.cc', benchmark_dep],
    'pmd_bench': ['pmd_bench.cc', benchmark_dep],
    'ring_bench': ['ring_bench.cc', benchmark_dep],
    'traffic_class_bench': ['traffic_class_bench.cc', benchmark_main_dep],
    'modules_url_filter_bench': ['modules/url_filter_bench.cc', benchmark_main_dep],
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
