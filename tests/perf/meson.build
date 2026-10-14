# live_perf (D-138): classified live performance tests, a client of the C++
# SDK alone. It starts its own bessd per round (sudo -n), so it is no meson test.
live_perf = executable(
  'live_perf',
  'live_perf.cc',
  implicit_include_directories: false,
  dependencies: [bess_sdk_dep],
)
