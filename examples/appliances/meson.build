project('appliances', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=1',
  ],
)

# Reference appliances (roadmap M24): applications on BESS's installed
# headers, each built as a plugin bessd loads. The application code (the
# *_app.h headers) also runs without a module graph: each module's
# `self_test` command drives it directly on frames.
bess_dep = dependency('bess-dev', required: true)
modules_dir = get_option('libdir') / 'bess' / 'modules'

foreach appliance : ['router', 'nat', 'vswitch']
  shared_module(
    appliance + '_appliance',
    appliance / appliance + '_appliance.cc',
    include_directories: include_directories('.'),
    dependencies: [bess_dep],
    install: true,
    install_dir: modules_dir,
  )
endforeach

# R4's resources are driven over the control API (D-094): its own wire types,
# compiled for the plugin's codecs, and their descriptor set installed next to
# the plugin so a controller can build the messages without generated code
# (tools/check_standalone_plugins.py loads it).
protoc = find_program('protoc', required: true)
protobuf_dep = dependency('protobuf', required: true)
session_proto = custom_target(
  'session_proto',
  input: 'session/session.proto',
  output: ['session.pb.cc', 'session.pb.h', 'session_appliance.desc'],
  command: [protoc, '--proto_path=@CURRENT_SOURCE_DIR@/session', '--cpp_out=@OUTDIR@',
            '--descriptor_set_out=@OUTPUT2@', '@INPUT@'],
  install: true,
  install_dir: [false, false, modules_dir],
)
shared_module(
  'session_appliance',
  ['session/session_appliance.cc', session_proto[0], session_proto[1]],
  include_directories: include_directories('.'),
  dependencies: [bess_dep, protobuf_dep],
  install: true,
  install_dir: modules_dir,
)
