proto_source_dir = meson.current_source_dir()
cpp_proto_root = join_paths(meson.current_build_dir(), 'generated', 'cpp')
python_proto_root = join_paths(meson.current_build_dir(), 'generated', 'python')
protobuf_generator = files('../tools/generate_protobuf.py')

core_proto_names = [
  'bess_msg.proto',
  'util_msg.proto',
  'service.proto',
  'module_msg.proto',
  'error.proto',
  'ports/port_msg.proto',
  'tests/test_msg.proto',
]
core_proto_files = files(
  'bess_msg.proto',
  'util_msg.proto',
  'service.proto',
  'module_msg.proto',
  'error.proto',
  'ports/port_msg.proto',
  'tests/test_msg.proto',
)

python_generated_install_entries = []
core_cpp_proto_targets = []
core_python_proto_targets = []

foreach proto_index : range(core_proto_names.length())
  proto_name = core_proto_names[proto_index]
  proto_stem = proto_name.replace('.proto', '')
  target_name = 'core_proto_' + proto_stem.replace('/', '_')
  cpp_proto_source_root = proto_source_dir
  if proto_stem.startswith('ports/') or proto_stem.startswith('tests/')
    cpp_proto_source_root = join_paths(proto_source_dir, proto_stem.split('/')[0])
  endif
  cpp_outputs = [
    'core_' + proto_stem.replace('/', '_') + '.pb.cc',
    'core_' + proto_stem.replace('/', '_') + '.pb.h',
    'core_' + proto_stem.replace('/', '_') + '.grpc.pb.cc',
    'core_' + proto_stem.replace('/', '_') + '.grpc.pb.h',
  ]
  cpp_target = custom_target(
    target_name + '_cpp',
    input: core_proto_files[proto_index],
    output: cpp_outputs,
    depend_files: core_proto_files,
    command: [
      python,
      protobuf_generator,
      protoc.full_path(),
      grpc_cpp_plugin.full_path(),
      'cpp',
      cpp_proto_source_root,
      join_paths(cpp_proto_root, 'core', 'pb'),
      '@INPUT@',
      '@OUTPUT@',
    ],
    build_by_default: true,
  )
  core_cpp_proto_targets += cpp_target

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

sample_proto_name = 'supdate_msg.proto'
sample_proto_file = files('../sample_plugin/protobuf/supdate_msg.proto')
sample_cpp_target = custom_target(
  'sample_proto_cpp',
  input: sample_proto_file,
  output: [
    'sample_supdate_msg.pb.cc',
    'sample_supdate_msg.pb.h',
    'sample_supdate_msg.grpc.pb.cc',
    'sample_supdate_msg.grpc.pb.h',
  ],
  depend_files: sample_proto_file,
  command: [
    python,
    protobuf_generator,
    protoc.full_path(),
    grpc_cpp_plugin.full_path(),
    'cpp',
    join_paths(meson.current_source_dir(), '../sample_plugin/protobuf'),
    join_paths(cpp_proto_root, 'sample', 'pb'),
    '@INPUT@',
    '@OUTPUT@',
  ],
  build_by_default: true,
)

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
python_proto_include = ['-I' + python_proto_root]

protobuf_runtime_deps = [
  dependency('protobuf', method: 'pkg-config', required: true),
  dependency('grpc++', method: 'pkg-config', required: true),
]

core_cpp_include = [
  '-I' + join_paths(cpp_proto_root, 'core', 'pb'),
  '-I' + join_paths(cpp_proto_root, 'core'),
]
bess_proto_lib = static_library(
  'bess_proto',
  core_cpp_proto_targets,
  cpp_args: core_cpp_include,
  dependencies: protobuf_runtime_deps,
)
bess_proto_dep = declare_dependency(
  link_with: bess_proto_lib,
  compile_args: core_cpp_include,
  dependencies: protobuf_runtime_deps,
)
sample_proto_include = [
  '-I' + join_paths(cpp_proto_root, 'sample'),
  '-I' + join_paths(cpp_proto_root, 'sample', 'pb'),
]
