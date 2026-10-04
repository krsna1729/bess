project('sdk_battery', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=2',
    'werror=true',
  ],
)

# Persona E (roadmap M23): a third-party library on installed BESS primitives
# only. Built and tested from the installed bess-dev package, without bessd.
bess_dep = dependency('bess-dev', required: true)

test('lease_table_test',
  executable('lease_table_test', 'lease_table_test.cc', dependencies: [bess_dep]))
