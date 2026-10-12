# The C++ bessctl (G1.7, D-104): on the public C++ SDK (libbess-sdk) only,
# never on bessd's internals. The Python files in this directory are the old
# bessctl and pybess's test harness; they go when D-104's last step does.

bessctl_lib = static_library(
  'bessctl_lib',
  'json.cc',
  'manifest.cc',
  dependencies: [bess_sdk_dep],
)
bessctl_dep = declare_dependency(link_with: bessctl_lib, dependencies: [bess_sdk_dep])

bessctl_exe = executable(
  'bessctl',
  'main.cc',
  dependencies: [bessctl_dep],
  install: true,
)

if get_option('build_tests')
  test('bessctl_manifest_test',
       executable('bessctl_manifest_test', 'manifest_test.cc',
                  dependencies: [bessctl_dep, gtest_dep,
                                 dependency('gtest_main', method: 'pkg-config')]),
       suite: 'bessctl')
endif
