# The sample plugin's generated C++ code, declared at the path its sources
# include ("pb/supdate_msg.pb.h"); see ../../pb/meson.build.
sample_pb_dir = meson.current_build_dir()
sample_cpp_target = custom_target(
  'sample_proto_cpp',
  input: sample_proto_file,
  output: [
    'supdate_msg.pb.cc',
    'supdate_msg.pb.h',
    'supdate_msg.grpc.pb.cc',
    'supdate_msg.grpc.pb.h',
  ],
  depend_files: sample_proto_file,
  command: [
    python,
    protobuf_generator,
    protoc.full_path(),
    grpc_cpp_plugin.full_path(),
    'cpp',
    join_paths(meson.project_source_root(), 'sample_plugin', 'protobuf'),
    sample_pb_dir,
    '@INPUT@',
    '@OUTPUT@',
  ],
  build_by_default: true,
)
