project('standalone_plugin', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=1',
  ],
)

bess_dep = dependency('bess-dev', required: true)

# Conformance plugins (roadmap M2): each is built from the installed `bess-dev`
# package only. standalone_pass is the trivial module, standalone_macswap
# manipulates packets, standalone_range_gate uses the public classifier, and
# standalone_flow_count keeps per-flow state in the public flow table (M23).
foreach plugin : ['standalone_pass', 'standalone_macswap', 'standalone_range_gate',
                  'standalone_flow_count']
  shared_module(
    plugin,
    plugin + '.cc',
    dependencies: [bess_dep],
    install: true,
    install_dir: get_option('libdir') / 'bess' / 'modules',
  )
endforeach
