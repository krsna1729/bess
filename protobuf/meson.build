proto_source_dir = meson.current_source_dir()
python_proto_root = join_paths(meson.current_build_dir(), 'generated', 'python')
protobuf_generator = files('../tools/generate_protobuf.py')

core_proto_names = [
  'bess_msg.proto',
  'util_msg.proto',
  'service.proto',
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
  'service.proto',
  'control_v2.proto',
  'module_msg.proto',
  'error.proto',
  'ports/port_msg.proto',
  'tests/test_msg.proto',
  'tests/ingress_bench.proto',
)

python_generated_install_entries = []
core_python_proto_targets = []
subdir('pb')  # the C++ code: core_cpp_proto_targets, core_pb_dir

foreach proto_index : range(core_proto_names.length())
  proto_name = core_proto_names[proto_index]
  proto_stem = proto_name.replace('.proto', '')
  target_name = 'core_proto_' + proto_stem.replace('/', '_')
  python_outputs = [
    'builtin_' + proto_stem.replace('/', '_') + '_pb2.py',
    'builtin_' + proto_stem.replace('/', '_') + '_pb2_grpc.py',
  ]
  python_proto_source_root = proto_source_dir
  if proto_stem.startswith('tests/')
    python_proto_source_root = join_paths(proto_source_dir, 'tests')
  endif
  python_target = custom_target(
    target_name + '_python',
    input: core_proto_files[proto_index],
    output: python_outputs,
    depend_files: core_proto_files,
    command: [
      python,
      protobuf_generator,
      protoc.full_path(),
      grpc_python_plugin.full_path(),
      'python',
      python_proto_source_root,
      join_paths(python_proto_root, 'builtin_pb'),
      '@INPUT@',
      '@OUTPUT@',
    ],
    build_by_default: true,
  )
  python_install_stem = proto_stem
  if proto_stem.startswith('tests/')
    python_install_stem = proto_stem.split('/')[-1]
  endif
  python_install_dir = 'builtin_pb'
  if proto_stem.startswith('ports/')
    python_install_dir = join_paths('builtin_pb', 'ports')
  endif
  python_generated_install_entries += [
    [
      join_paths(python_proto_root, 'builtin_pb',
                 python_install_stem + '_pb2.py'),
      python_install_dir,
    ],
    [
      join_paths(python_proto_root, 'builtin_pb',
                 python_install_stem + '_pb2_grpc.py'),
      python_install_dir,
    ],
  ]
  core_python_proto_targets += python_target
endforeach

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
subdir('sample/pb')  # sample_cpp_target

sample_python_target = custom_target(
  'sample_proto_python',
  input: sample_proto_file,
  output: [
    'plugin_supdate_msg_pb2.py',
    'plugin_supdate_msg_pb2_grpc.py',
  ],
  depend_files: sample_proto_file,
  command: [
    python,
    protobuf_generator,
    protoc.full_path(),
    grpc_python_plugin.full_path(),
    'python',
    join_paths(meson.current_source_dir(), '../sample_plugin/protobuf'),
    join_paths(python_proto_root, 'plugin_pb'),
    '@INPUT@',
    '@OUTPUT@',
  ],
  build_by_default: true,
)

python_generated_install_entries += [
  [
    join_paths(python_proto_root, 'plugin_pb', 'supdate_msg_pb2.py'),
    'plugin_pb',
  ],
  [
    join_paths(python_proto_root, 'plugin_pb', 'supdate_msg_pb2_grpc.py'),
    'plugin_pb',
  ],
]

copy_init = files('../tools/copy_init.py')
python_package_inits = [
  custom_target(
    'builtin_pb_init',
    input: files('../pybess/builtin_pb/__init__.py'),
    output: 'builtin_pb_init.py',
    command: [
      python,
      copy_init,
      '@INPUT@',
      join_paths(python_proto_root, 'builtin_pb', '__init__.py'),
      '@OUTPUT@',
    ],
    build_by_default: true,
  ),
  custom_target(
    'builtin_pb_ports_init',
    input: files('../pybess/builtin_pb/ports/__init__.py'),
    output: 'builtin_pb_ports_init.py',
    command: [
      python,
      copy_init,
      '@INPUT@',
      join_paths(python_proto_root, 'builtin_pb', 'ports', '__init__.py'),
      '@OUTPUT@',
    ],
    build_by_default: true,
  ),
  custom_target(
    'plugin_pb_init',
    input: files('../pybess/plugin_pb/__init__.py'),
    output: 'plugin_pb_init.py',
    command: [
      python,
      copy_init,
      '@INPUT@',
      join_paths(python_proto_root, 'plugin_pb', '__init__.py'),
      '@OUTPUT@',
    ],
    build_by_default: true,
  ),
  custom_target(
    'plugin_pb_ports_init',
    input: files('../pybess/plugin_pb/ports/__init__.py'),
    output: 'plugin_pb_ports_init.py',
    command: [
      python,
      copy_init,
      '@INPUT@',
      join_paths(python_proto_root, 'plugin_pb', 'ports', '__init__.py'),
      '@OUTPUT@',
    ],
    build_by_default: true,
  ),
]

python_proto_targets = core_python_proto_targets + [sample_python_target] + python_package_inits

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
