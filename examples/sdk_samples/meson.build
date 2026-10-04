project('sdk_samples', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=2',
    'werror=true',
  ],
)

# API samples (roadmap M23): the smallest meaningful program per public
# library -- typed use, an update, error handling, ownership -- compiled from
# the installed bess-dev alone. They are compiled, not linked: the libraries
# compiled into bessd (route, meter, rcu, flow) resolve their symbols from
# bessd when a plugin loads (examples/standalone_plugin runs that path).
bess_dep = dependency('bess-dev', required: true)

static_library('sdk_samples',
  files(
    'handoff_tunnel_sample.cc',
    'l2_sample.cc',
    'meter_sample.cc',
    'route_sample.cc',
    'stateful_sample.cc',
  ),
  dependencies: [bess_dep],
)
