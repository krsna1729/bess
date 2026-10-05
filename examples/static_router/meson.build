project('static_router', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=2',
    'werror=true',
  ],
)

# Decision D-095: R1's application (examples/appliances/router) as a program
# without bessd, linked from the installed archives of route, the dataplane
# core and RCU (bess-dev-static) and DPDK. CI's install check builds and runs
# it against the staged install.
bess_static_dep = dependency('bess-dev-static', required: true)

test('static_router',
  executable('static_router', 'static_router.cc',
    include_directories: include_directories('../appliances'),
    dependencies: [bess_static_dep]))
