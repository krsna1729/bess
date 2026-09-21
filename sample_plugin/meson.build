sample_plugin = shared_module(
  'sequential_update',
  [
    'modules/sequential_update.cc',
    sample_cpp_target,
  ],
  include_directories: core_include,
  cpp_args: core_cpp_include + sample_proto_include,
  dependencies: common_deps + [
    dependency('protobuf', method: 'pkg-config', required: true),
    dependency('grpc++', method: 'pkg-config', required: true),
  ],
  build_rpath: dpdk_libdir,
  install: true,
  install_dir: join_paths(get_option('libdir'), 'bess', 'modules'),
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
  depends: [sample_plugin] + python_proto_targets,
  env: sample_plugin_test_env,
  suite: 'plugin',
  is_parallel: false,
  timeout: 60,
)
