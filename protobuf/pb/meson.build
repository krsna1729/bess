# The generated C++ protocol code. Its declared outputs are the very files the
# compiles include ("pb/module_msg.pb.h" under -I <build>/protobuf, and
# "module_msg.pb.h" from a generated file's own directory). Ninja therefore sees
# that a .proto edit changes the header, and rebuilds every object that
# includes it in the same run.
core_pb_dir = meson.current_build_dir()
core_cpp_proto_targets = []
foreach proto_index : range(core_proto_names.length())
  proto_stem = core_proto_names[proto_index].replace('.proto', '')
  name = proto_stem.split('/')[-1]
  cpp_proto_source_root = proto_source_dir
  if proto_stem.startswith('ports/') or proto_stem.startswith('tests/')
    cpp_proto_source_root = join_paths(proto_source_dir, proto_stem.split('/')[0])
  endif
  cpp_target = custom_target(
    'core_proto_' + proto_stem.replace('/', '_') + '_cpp',
    input: core_proto_files[proto_index],
    output: [name + '.pb.cc', name + '.pb.h', name + '.grpc.pb.cc', name + '.grpc.pb.h'],
    depend_files: core_proto_files,
    command: [
      python,
      protobuf_generator,
      protoc.full_path(),
      grpc_cpp_plugin.full_path(),
      'cpp',
      cpp_proto_source_root,
      core_pb_dir,
      '@INPUT@',
      '@OUTPUT@',
    ],
    build_by_default: true,
  )
  core_cpp_proto_targets += cpp_target
  if proto_stem == 'control_v2'
    control_v2_cpp_target = cpp_target  # the C++ SDK's only generated code
  endif
endforeach
