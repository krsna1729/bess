python_proto_root = join_paths(meson.project_build_root(), 'protobuf', 'generated', 'python')
python_env = {
  'BESS_PROTOBUF_ROOT': python_proto_root,
  'PYTHONPATH': ':'.join([
    python_proto_root,
    join_paths(python_proto_root, 'builtin_pb'),
    join_paths(python_proto_root, 'plugin_pb'),
    meson.project_source_root(),
  ]),
  'BESSD_BINARY': join_paths(meson.project_build_root(), 'core', 'bessd'),
  'LD_LIBRARY_PATH': dpdk_libdir,
}

test(
  'python_unittest_discover',
  python,
  args: [
    '-m', 'unittest', 'discover', '-v',
    '-s', meson.project_source_root(),
  ],
  depends: python_proto_targets,
  env: python_env,
  suite: 'python',
  is_parallel: false,
  timeout: 1800,
)

integration_env = python_env
test(
  'module_integration',
  python,
  args: [join_paths(meson.project_source_root(), 'bessctl', 'run_module_tests.py')],
  depends: python_proto_targets,
  env: integration_env,
  suite: 'integration',
  is_parallel: false,
  timeout: 1800,
)
bess_data_dir = join_paths(get_option('datadir'), 'bess')
install_data(
  files('__init__.py', 'bess.py', 'module.py', 'pm_import.py',
        'port.py', 'protobuf_to_dict.py'),
  install_dir: join_paths(bess_data_dir, 'pybess'),
)
install_data(
  files('builtin_pb/__init__.py'),
  install_dir: join_paths(bess_data_dir, 'builtin_pb'),
)
install_data(
  files('builtin_pb/ports/__init__.py'),
  install_dir: join_paths(bess_data_dir, 'builtin_pb', 'ports'),
)
install_data(
  files('plugin_pb/__init__.py'),
  install_dir: join_paths(bess_data_dir, 'plugin_pb'),
)
install_data(
  files('plugin_pb/ports/__init__.py'),
  install_dir: join_paths(bess_data_dir, 'plugin_pb', 'ports'),
)
install_copy = find_program('cp', required: true)
foreach generated_entry : python_generated_install_entries
  output_name = generated_entry[0].split('/')[-1]
  custom_target(
    'install_' + generated_entry[1].replace('/', '_') + '_' + output_name,
    input: python_proto_targets,
    output: output_name,
    command: [install_copy, generated_entry[0], '@OUTPUT@'],
    install: true,
    install_dir: join_paths(bess_data_dir, generated_entry[1]),
  )
endforeach
