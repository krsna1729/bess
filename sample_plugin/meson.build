sample_plugin = shared_module(
  'sequential_update',
  [
    'modules/sequential_update.cc',
    'modules/usage_report.cc',
    sample_cpp_target,
    sample_usage_cpp_target,
    proto_ready_marker,  # core pb/*.pb.h, reached through module.h
  ],
  include_directories: [core_include, core_pb_inc, sample_pb_inc],
  dependencies: [dpdk_dep, glog_dep, protobuf_dep, grpcpp_dep],
  # A plugin must stay unloadable. GCC's default -fgnu-unique gives a static
  # local inside an inline function STB_GNU_UNIQUE binding, and protobuf's
  # generated enum helpers instantiate one (NameOfDenseEnum's static string)
  # as soon as a module calls, say, UsageReportMode_Name(). glibc then marks
  # the dlopen'ed object NODELETE at load, so dlclose() never unmaps it and
  # UnloadPlugin refuses (plugin_loader.cc's "stays mapped after dlclose") --
  # a module that merely names its own enum in GetDesc() makes the whole
  # plugin permanent. The flag costs a plugin nothing: it has no other object
  # to share the symbol with.
  cpp_args: cpp.get_supported_arguments('-fno-gnu-unique'),
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
  # The module itself in process (core/testing/module_harness.h): its source
  # linked into the test with the core modules, as a core module test is.
  sample_plugin_unit = executable(
    'sample_plugin_sequential_update_test',
    [
      'modules/sequential_update.cc',
      'modules/sequential_update_test.cc',
      files('../core/gtest_main.cc'),
      sample_cpp_target,
      proto_ready_marker,
    ],
    include_directories: [core_include, core_pb_inc, sample_pb_inc],
    cpp_args: unit_test_cpp_args,
    link_whole: [module_runtime_whole, module_harness_lib],
    dependencies: runtime_deps + [gtest_dep],
    build_rpath: runtime_rpath,
  )
  test(
    'sample_plugin_sequential_update',
    sample_plugin_unit,
    is_parallel: false,
    env: {'LD_LIBRARY_PATH': dpdk_libdir},
  )
endif
