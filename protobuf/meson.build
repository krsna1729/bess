proto_source_dir = meson.current_source_dir()
protobuf_generator = files('../tools/generate_protobuf.py')

core_proto_names = [
  'bess_msg.proto',
  'util_msg.proto',
  'control_v2.proto',
  'module_msg.proto',
  'error.proto',
  'ports/port_msg.proto',
  'tests/test_msg.proto',
  'tests/ingress_bench.proto',
]
core_proto_files = files(
  'bess_msg.proto',
  'util_msg.proto',
  'control_v2.proto',
  'module_msg.proto',
  'error.proto',
  'ports/port_msg.proto',
  'tests/test_msg.proto',
  'tests/ingress_bench.proto',
)

subdir('pb')  # the C++ code: core_cpp_proto_targets, core_pb_dir

proto_ready_marker = custom_target(
  'core_proto_ready',
  input: core_cpp_proto_targets,
  output: 'core_proto_ready.h',
  command: [find_program('touch'), '@OUTPUT@'],
  build_by_default: true,
)
# bess-dev installs the generated headers the public C++ headers include,
# transitively, and nothing else: no *.grpc.pb.h, no .pb.cc, no control or test
# protocol. tools/public_proto_closure.py computes the set from the include
# graph and tools/api_classes.json names it; the test below and the installed
# tree check (tools/check_installed_headers.py) fail if this list drifts.
public_pb_headers = ['bess_msg', 'error', 'module_msg', 'port_msg', 'util_msg']
pb_install_copy = find_program('cp')
foreach stem : public_pb_headers
  custom_target(
    'install_pb_' + stem,
    input: core_cpp_proto_targets,
    output: stem + '.pb.h',
    command: [pb_install_copy,
              join_paths(core_pb_dir, stem + '.pb.h'),
              '@OUTPUT@'],
    build_by_default: true,
    install: true,
    install_dir: join_paths(get_option('includedir'), 'bess', 'core', 'pb'),
  )
endforeach
test(
  'check_public_proto_closure',
  python,
  args: [files('../tools/public_proto_closure.py'),
         '--core-dir', join_paths(meson.project_source_root(), 'core'),
         '--pb-dir', core_pb_dir,
         '--check'],
  depends: core_cpp_proto_targets,
  is_parallel: false,
  suite: 'architecture',
)
test(
  'check_installed_headers_self_test',
  python,
  args: [files('../tools/check_installed_headers.py'), '--self-test'],
  is_parallel: false,
  suite: 'architecture',
)


sample_proto_name = 'supdate_msg.proto'
sample_proto_file = files('../sample_plugin/protobuf/supdate_msg.proto')
# The worked example's message: usage_report.h includes "pb/usage_msg.pb.h".
usage_proto_file = files('../sample_plugin/protobuf/usage_msg.proto')
subdir('sample/pb')  # sample_cpp_target, sample_usage_cpp_target

# The Python client (pybess, the Python bessctl, its generated protobuf
# packages) left with D-140: the C++ code above is the whole protobuf build.


protobuf_runtime_deps = [
  protobuf_dep,
  grpcpp_dep,
]

# include_directories objects, not '-I' + build-dir strings: meson emits them
# relative to the build directory, so the compiler's depfile names a generated
# header by the same path as the custom_target that writes it. With absolute
# -I paths the depfile's header was a different ninja node that no edge
# produced, and a .proto edit left its includers stale for one ninja run.
core_pb_inc = include_directories(
  'pb',  # "x.pb.h"
  '.',  # "pb/x.pb.h"
)
# protoc-generated code is not ours: newer GCCs report false-positive
# (maybe-)uninitialized warnings inside protobuf's own headers at -O3, which
# -Werror would turn into a failed release build. Keep them warnings here.
proto_codegen_args = cpp.get_supported_arguments(
  '-Wno-error=uninitialized',
  '-Wno-error=maybe-uninitialized',
)
bess_proto_lib = static_library(
  'bess_proto',
  core_cpp_proto_targets,
  include_directories: core_pb_inc,
  cpp_args: proto_codegen_args,
  dependencies: protobuf_runtime_deps,
)
bess_proto_headers_dep = declare_dependency(
  include_directories: core_pb_inc,
  dependencies: [protobuf_dep],
)
bess_proto_dep = declare_dependency(
  link_with: bess_proto_lib,
  include_directories: core_pb_inc,
  dependencies: protobuf_runtime_deps,
)
sample_pb_inc = include_directories(
  'sample',  # "pb/supdate_msg.pb.h"
  'sample/pb',
)
