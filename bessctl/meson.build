# The C++ bessctl (G1.7, D-104): on the public C++ SDK (libbess-sdk) only,
# never on bessd's internals. The Python files in this directory are the old
# bessctl and pybess's test harness; they go when D-104's last step does.
#
# The shipped bessctl is bessd itself (D-120): bessd links bess_ctl and the
# SDK built without its own control_v2 code (bess_sdk_embedded); installed,
# bessctl is a symlink to bessd. bessctl-standalone, on libbess-sdk, is the
# client alone, built but not installed.

# YAML manifests (yaml_io.h): libyaml, linked statically in a static build.
yaml_dep = dependency('yaml-0.1', method: 'pkg-config', static: is_static_deps, required: true)

# The client's code against the SDK's headers only: each executable picks the
# SDK library it links.
bess_ctl_lib = static_library(
  'bess_ctl',
  'json.cc',
  'manifest.cc',
  'yaml_io.cc',
  'main.cc',
  dependencies: [bess_sdk_dep.partial_dependency(compile_args: true, includes: true,
                                                 sources: true),
                 yaml_dep],
)
bessctl_dep = declare_dependency(link_with: bess_ctl_lib, dependencies: [bess_sdk_dep, yaml_dep])
# For bessd (core/meson.build), which links control_v2 in bess_proto. The
# include directory is the source root: bessd includes "bessctl/cli.h".
bessctl_embedded_dep = declare_dependency(
  link_with: [bess_ctl_lib, bess_sdk_embedded_lib],
  include_directories: include_directories('..'),
  dependencies: [yaml_dep, protobuf_dep, grpcpp_dep],
)

bessctl_standalone = executable(
  'bessctl-standalone',
  'standalone_main.cc',
  dependencies: [bessctl_dep],
  link_args: static_exe_link_args,
)

# The build directory's bessctl is what installs: a symlink to bessd. bessd
# is not built yet when this is read; whoever runs the link needs bessd too.
bessctl_link = custom_target(
  'bessctl',
  output: 'bessctl',
  command: [find_program('ln'), '-sfn', join_paths('..', 'core', 'bessd'), '@OUTPUT@'],
  build_by_default: true,
)

if get_option('build_tests')
  test('bessctl_manifest_test',
       executable('bessctl_manifest_test', 'manifest_test.cc',
                  dependencies: [bessctl_dep, gtest_dep,
                                 dependency('gtest_main', method: 'pkg-config')]),
       suite: 'bessctl')
endif
