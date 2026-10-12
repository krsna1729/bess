# The C++ control SDK (G1.6, D-104). Two libraries from the same sources:
#  - bess_sdk_core: the client code alone. A program that already links
#    BESS's generated control_v2 code (bessd, for the bessctl it carries)
#    links this, so the descriptors are registered once.
#  - libbess-sdk (installed, pkg-config bess-sdk): the client plus the
#    control_v2 generated code, for programs outside the tree.
# Neither sees core/: the SDK depends on the protocol, not on bessd.

sdk_cpp_inc = include_directories('include')
sdk_cpp_sources = files('src/client.cc', 'src/grpc_transport.cc')
sdk_cpp_args = core_cpp_include + proto_codegen_args

bess_sdk_core = static_library(
  'bess_sdk_core',
  sdk_cpp_sources,
  control_v2_cpp_target[1], control_v2_cpp_target[3],  # headers: build order only
  include_directories: sdk_cpp_inc,
  cpp_args: sdk_cpp_args,
  dependencies: [protobuf_dep, grpcpp_dep],
)
bess_sdk_core_dep = declare_dependency(
  link_with: bess_sdk_core,
  include_directories: sdk_cpp_inc,
  compile_args: core_cpp_include,
  dependencies: [protobuf_dep, grpcpp_dep],
)

# A static bessd links its dependencies' archives, which need not be PIC:
# the SDK is then a static archive too.
sdk_lib_kwargs = {
  'include_directories': sdk_cpp_inc,
  'cpp_args': sdk_cpp_args,
  'dependencies': [protobuf_dep, grpcpp_dep],
  'install': true,
}
if is_static_deps
  bess_sdk_lib = static_library('bess-sdk', sdk_cpp_sources, control_v2_cpp_target,
                                kwargs: sdk_lib_kwargs)
else
  bess_sdk_lib = library('bess-sdk', sdk_cpp_sources, control_v2_cpp_target,
                         kwargs: sdk_lib_kwargs)
endif
bess_sdk_dep = declare_dependency(
  link_with: bess_sdk_lib,
  include_directories: sdk_cpp_inc,
  compile_args: core_cpp_include,
  dependencies: [protobuf_dep, grpcpp_dep],
)

install_headers('include/bess/sdk/client.h', 'include/bess/sdk/transport.h',
                subdir: 'bess/sdk')
# The public headers include "control_v2.pb.h" from their own directory.
custom_target(
  'install_sdk_control_v2_pb_h',
  input: control_v2_cpp_target,
  output: 'control_v2.pb.h',
  command: [find_program('cp'), join_paths(cpp_proto_root, 'core', 'pb', 'control_v2.pb.h'),
            '@OUTPUT@'],
  build_by_default: true,
  install: true,
  install_dir: join_paths(get_option('includedir'), 'bess', 'sdk'),
)
pkg.generate(
  bess_sdk_lib,
  name: 'bess-sdk',
  filebase: 'bess-sdk',
  description: 'BESS control API client (control_v2)',
  version: meson.project_version(),
  requires: ['grpc++', 'protobuf'],
  extra_cflags: ['-std=c++23'],
)

# The live test program is what bessctl/module_tests/control_sdk_cpp.py runs
# against a live bessd (pybess/meson.build passes its path).
sdk_cpp_test_targets = []
if get_option('build_tests')
  sdk_cpp_test = executable(
    'sdk_cpp_client_test',
    'client_test.cc',
    dependencies: [bess_sdk_dep, gtest_dep, dependency('gtest_main', method: 'pkg-config')],
  )
  test('sdk_cpp_client_test', sdk_cpp_test, suite: 'sdk')
  sdk_cpp_live_test = executable(
    'sdk_cpp_live_test',
    'live_test.cc',
    dependencies: [bess_sdk_dep],
  )
  sdk_cpp_test_targets += sdk_cpp_live_test
endif
