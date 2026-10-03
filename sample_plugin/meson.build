sample_plugin = shared_module(
  'sequential_update',
  [
    'modules/sequential_update.cc',
    sample_cpp_target,
    proto_ready_marker,  # core pb/*.pb.h, reached through module.h
  ],
  include_directories: core_include,
  cpp_args: core_cpp_include + sample_proto_include,
  dependencies: [dpdk_dep, glog_dep, protobuf_dep, grpcpp_dep],
  build_rpath: dpdk_libdir,
  install: true,
  install_dir: join_paths(get_option('libdir'), 'bess', 'modules'),
)
# Test-only: declares an unsupported API range; the daemon must refuse it.
# It must not carry sample_cpp_target: dlopen() runs the generated file's
# static registration before the daemon can refuse the plugin, and dlclose()
# does not undo it, so a probe loaded before sequential_update (readdir order)
# made that plugin's identical registration abort the daemon.
incompatible_probe = shared_module(
  'incompatible_probe',
  ['modules/incompatible_probe.cc', proto_ready_marker],
  include_directories: core_include,
  cpp_args: core_cpp_include + sample_proto_include,
  dependencies: [dpdk_dep, glog_dep, protobuf_dep, grpcpp_dep],
  build_rpath: dpdk_libdir,
  install: false,
)
sample_plugin_test_env = {
  'BESS_PROTOBUF_ROOT': join_paths(meson.project_build_root(), 'protobuf', 'generated', 'python'),
  'PYTHONPATH': ':'.join([
    join_paths(meson.project_build_root(), 'protobuf', 'generated', 'python'),
    join_paths(meson.project_build_root(), 'protobuf', 'generated', 'python', 'builtin_pb'),
    join_paths(meson.project_build_root(), 'protobuf', 'generated', 'python', 'plugin_pb'),
    meson.project_source_root(),
  ]),
  'LD_LIBRARY_PATH': dpdk_libdir,
}
test(
  'sample_plugin_load',
  python,
  args: [
    join_paths(meson.project_source_root(), 'tools', 'check_sample_plugin.py'),
    '--bessd', join_paths(meson.project_build_root(), 'core', 'bessd'),
    '--plugin-dir', meson.current_build_dir(),
  ],
  depends: [sample_plugin, incompatible_probe] + python_proto_targets,
  env: sample_plugin_test_env,
  suite: 'plugin',
  is_parallel: false,
  timeout: 60,
)
