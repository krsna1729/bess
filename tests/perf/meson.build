# The live performance clients, on the C++ SDK alone (no meson tests: they
# need a bessd on isolated CPUs).
# live_perf (D-138): classified live performance tests; starts its own bessd
# per round (sudo -n).
live_perf = executable(
  'live_perf',
  'live_perf.cc',
  implicit_include_directories: false,
  include_directories: include_directories('.'),
  dependencies: [bess_sdk_dep],
)
# live_transaction_bench (D-139): packet rate while transactions change rules,
# against a running bessd.
live_transaction_bench = executable(
  'live_transaction_bench',
  'live_transaction_bench.cc',
  implicit_include_directories: false,
  include_directories: include_directories('.'),
  dependencies: [bess_sdk_dep],
)
