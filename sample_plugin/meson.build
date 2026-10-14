sample_plugin = shared_module(
  'sequential_update',
  [
    'modules/sequential_update.cc',
    sample_cpp_target,
    proto_ready_marker,  # core pb/*.pb.h, reached through module.h
  ],
  include_directories: [core_include, core_pb_inc, sample_pb_inc],
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
  include_directories: [core_include, core_pb_inc, sample_pb_inc],
  dependencies: [dpdk_dep, glog_dep, protobuf_dep, grpcpp_dep],
  build_rpath: dpdk_libdir,
  install: false,
)
# The plugin in a daemon (tests/live/sample_plugin_test.cc): loaded with
# --modules, the probe refused, the shipped manifest run, unloaded only once
# unused. In the `plugin` suite, which every lane runs.
if get_option('build_tests')
  live_sample_plugin = executable(
    'live_sample_plugin',
    files('../tests/live/sample_plugin_test.cc'),
    implicit_include_directories: false,
    link_with: live_lib,
    dependencies: [bess_sdk_dep, gtest_dep, dependency('gtest_main', method: 'pkg-config')],
  )
  test(
    'sample_plugin_load',
    live_sample_plugin,
    depends: [bessd, bessctl_link, sample_plugin, incompatible_probe],
    env: live_env + {'BESS_PLUGIN_DIR': meson.current_build_dir()},
    suite: 'plugin',
    is_parallel: false,
    timeout: 120,
  )
endif
