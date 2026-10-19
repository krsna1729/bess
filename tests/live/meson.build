# Live tests (tests/live/live_daemon.h): each binary starts one unprivileged
# bessd and drives it through the C++ SDK and bessctl. No Python.
live_test_sources = [
  'bessctl_test.cc',
  'classifier_traffic_test.cc',
  'conformance_test.cc',
  'control_sdk_test.cc',
  'dataplane_transactions_test.cc',
  'events_test.cc',
  'http_test.cc',
  'match_modules_test.cc',
  'metrics_test.cc',
  'module_traffic_test.cc',
  'simd_width_test.cc',
  'stateful_modules_test.cc',
  'usage_report_test.cc',
]
live_lib = static_library(
  'live_daemon',
  'live_daemon.cc',
  implicit_include_directories: false,
  # glog is explicit: the harness logs (LOG(INFO)/LOG(ERROR)) and used to get
  # the macro from a header protobuf happens to pull in on some toolchains --
  # CI's did not define it, and every lane failed to build.
  dependencies: [bess_sdk_dep, gtest_dep, glog_dep],
)
# packet_builder.h (header-only, std only) for test packets; simd_width_test
# also reads the CPU's vector features from arch/simd_dispatch.h (installed,
# self-contained), the way bessd does, to know the width to expect.
live_inc = include_directories('../../core/testing')
live_arch_inc = include_directories('../../core')
live_env = {
  'BESSD': bessd.full_path(),
  'BESSCTL': bessctl_link.full_path(),  # bessd, as installed (D-120)
  'BESS_SDK_CPP_CONFORMANCE': join_paths(meson.project_build_root(), 'sdk', 'cpp',
                                         'sdk_cpp_conformance'),
  'BESS_SOURCE_ROOT': meson.project_source_root(),
  'LD_LIBRARY_PATH': dpdk_libdir,
}
foreach source : live_test_sources
  name = 'live_' + source.replace('_test.cc', '')
  extra_sources = []
  includes = live_inc
  env = live_env
  if source == 'simd_width_test.cc'
    includes = [live_inc, live_arch_inc]
  elif source == 'usage_report_test.cc'
    # The sample plugin's own message (the test builds UsageReportArg and the
    # quotas resource with the generated types) and the plugin itself: the
    # daemon it starts loads it with --modules.
    extra_sources = [sample_usage_cpp_target]
    includes = [live_inc, sample_pb_inc]
    env = live_env + {'BESS_PLUGIN_DIR': join_paths(meson.project_build_root(), 'sample_plugin')}
  endif
  exe = executable(
    name,
    [source] + extra_sources,
    implicit_include_directories: false,
    include_directories: includes,
    link_with: live_lib,
    dependencies: [bess_sdk_dep, gtest_dep, dependency('gtest_main', method: 'pkg-config')],
  )
  test(
    name,
    exe,
    depends: [bessd, bessctl_link] + sdk_cpp_test_targets,
    env: env,
    suite: 'live',
    is_parallel: false,
    timeout: 600,
  )
endforeach
# Plugins built from the installed tree in the installed bessd: ci_profile
# verify-install runs it with BESSD, BESSCTL and BESS_PLUGIN_DIR set (the
# plugins exist only there), so it is no meson test.
live_standalone_plugins = executable(
  'live_standalone_plugins',
  'standalone_plugins_test.cc',
  implicit_include_directories: false,
  link_with: live_lib,
  dependencies: [bess_sdk_dep, gtest_dep, dependency('gtest_main', method: 'pkg-config')],
)
