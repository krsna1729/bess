# Live tests (tests/live/live_daemon.h): each binary starts one unprivileged
# bessd and drives it through the C++ SDK and bessctl. No Python.
live_test_sources = [
  'bessctl_test.cc',
]

live_lib = static_library(
  'live_daemon',
  'live_daemon.cc',
  implicit_include_directories: false,
  dependencies: [bess_sdk_dep, gtest_dep],
)
live_env = {
  'BESSD': bessd.full_path(),
  'BESSCTL': bessctl_exe.full_path(),
  'BESS_SDK_CPP_CONFORMANCE': join_paths(meson.project_build_root(), 'sdk', 'cpp',
                                         'sdk_cpp_conformance'),
  'BESS_SOURCE_ROOT': meson.project_source_root(),
  'LD_LIBRARY_PATH': dpdk_libdir,
}
foreach source : live_test_sources
  name = 'live_' + source.replace('_test.cc', '')
  exe = executable(
    name,
    source,
    implicit_include_directories: false,
    link_with: live_lib,
    dependencies: [bess_sdk_dep, gtest_dep, dependency('gtest_main', method: 'pkg-config')],
  )
  test(
    name,
    exe,
    depends: [bessd, bessctl_exe] + sdk_cpp_test_targets,
    env: live_env,
    suite: 'live',
    is_parallel: false,
    timeout: 600,
  )
endforeach
