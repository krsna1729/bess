# The C++ bessctl (G1.7, D-104): on the public C++ SDK (libbess-sdk) only,
# never on bessd's internals. The Python files in this directory are the old
# bessctl and pybess's test harness; they go when D-104's last step does.

# YAML manifests (yaml_io.h): libyaml, linked statically in a static build.
yaml_dep = dependency('yaml-0.1', method: 'pkg-config', static: is_static_deps, required: true)

bessctl_lib = static_library(
  'bessctl_lib',
  'json.cc',
  'manifest.cc',
  'yaml_io.cc',
  dependencies: [bess_sdk_dep, yaml_dep],
)
bessctl_dep = declare_dependency(link_with: bessctl_lib, dependencies: [bess_sdk_dep, yaml_dep])

bessctl_exe = executable(
  'bessctl',
  'main.cc',
  dependencies: [bessctl_dep],
  link_args: static_exe_link_args,
  install: true,
)

if get_option('build_tests')
  test('bessctl_manifest_test',
       executable('bessctl_manifest_test', 'manifest_test.cc',
                  dependencies: [bessctl_dep, gtest_dep,
                                 dependency('gtest_main', method: 'pkg-config')]),
       suite: 'bessctl')
endif
