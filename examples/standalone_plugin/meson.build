project('standalone_plugin', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=1',
  ],
)

bess_dep = dependency('bess-dev', required: true)

shared_module(
  'standalone_pass',
  'standalone_pass.cc',
  dependencies: [bess_dep],
  install: true,
  install_dir: get_option('libdir') / 'bess' / 'modules',
)
