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

shared_module(
  'router_appliance',
  'router/router_appliance.cc',
  include_directories: include_directories('.'),
  dependencies: [bess_dep],
  install: true,
  install_dir: get_option('libdir') / 'bess' / 'modules',
)
