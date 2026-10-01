project(
  'bess',
  'cpp',
  version: '0.1.0',
  meson_version: '>=1.2.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=0',
    'buildtype=release',
  ],
)
if meson.project_source_root() == meson.project_build_root()
  error('In-source builds are disallowed. Please use a separate build directory, e.g. "meson setup build-meson".')
endif


cpp = meson.get_compiler('cpp')
python = import('python').find_installation('python3')
fs = import('fs')

cpu = get_option('cpu')
if cpu != ''
  add_project_arguments('-march=' + cpu, language: 'cpp')
endif

add_project_arguments(
  '-D_GNU_SOURCE',
  '-DGLOG_USE_GLOG_EXPORT',
  '-include', 'cinttypes',
  '-Wall',
  '-Wextra',
  '-Werror',
  '-Wcast-align',
  # DPDK headers contain intentional unaligned casts; retain the warning without
  # making external-header diagnostics fail the build.
  '-Wno-error=cast-align',
  '-Wno-pedantic',
  '-Wno-error=deprecated-declarations',
  '-Wno-error=array-bounds',
  '-Wno-error=address-of-packed-member',
  '-Wno-error=unused-result',
  '-Wno-error=unused-variable',
  '-Wno-error=unused-but-set-variable',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'cpp',
)
static_binary_opt = get_option('static_binary')
is_static_deps = (static_binary_opt != 'none')
dpdk_is_static = (get_option('dpdk_link') == 'static') or is_static_deps

# libdpdk.pc carries the DPDK build machine's -march and -mrtm flags. They
# must not override BESS's explicit cpu option, especially in portable CI.
dpdk_raw = dependency(
  'libdpdk',
  method: 'pkg-config',
  static: dpdk_is_static,
  required: true,
)
pkg_config = find_program('pkg-config', required: true)
pkg_config_env = environment()
pkg_config_paths = get_option('pkg_config_path')
if pkg_config_paths.length() > 0
  pkg_config_env.set('PKG_CONFIG_PATH', ':'.join(pkg_config_paths))
endif

dpdk_compile_args = []
foreach arg : run_command(
  pkg_config, '--cflags', 'libdpdk',
  env: pkg_config_env,
  check: true,
).stdout().strip().split(' ')
  if arg != '' and not arg.startswith('-march=') and arg != '-mrtm'
    dpdk_compile_args += arg
  endif
endforeach
dpdk_pkg_link_args = ['--libs', 'libdpdk']
if dpdk_is_static
  dpdk_pkg_link_args = ['--libs', '--static', 'libdpdk']
endif
dpdk_link_args = []
foreach arg : run_command(
  pkg_config, dpdk_pkg_link_args,
  env: pkg_config_env,
  check: true,
).stdout().strip().split(' ')
  if arg != ''
    dpdk_link_args += arg
  endif
endforeach
dpdk_dep = declare_dependency(
  compile_args: dpdk_compile_args,
  link_args: dpdk_link_args,
)

dpdk_libdir = run_command(
  pkg_config, '--variable=libdir', 'libdpdk',
  env: pkg_config_env,
  check: true,
).stdout().strip()

if get_option('af_xdp') == 'required'
  dependency('libxdp', version: '>=1.2.2', method: 'pkg-config', required: true)
  dependency('libbpf', method: 'pkg-config', required: true)
  if not cpp.has_header('xdp/xsk.h') or not cpp.has_header('bpf/bpf.h')
    error('AF_XDP is required but libxdp/libbpf headers are unavailable')
  endif
  foreach artifact : ['librte_net_af_xdp.so', 'librte_net_af_xdp.a']
    if not fs.is_file(join_paths(dpdk_libdir, artifact))
      error('AF_XDP is required but DPDK is missing ' + join_paths(dpdk_libdir, artifact))
    endif
  endforeach
endif

glog_dep = dependency('libglog', method: 'pkg-config', static: is_static_deps, required: true)
# gflags must match glog's shared/static state to avoid double registration in gflags
gflags_dep = dependency('gflags', method: 'pkg-config', static: false, required: true)
protobuf_dep = dependency('protobuf', method: 'pkg-config', static: is_static_deps, required: true)
grpcpp_dep = dependency('grpc++', method: 'pkg-config', static: is_static_deps, required: true)
libunwind_dep = dependency('libunwind', method: 'pkg-config', static: is_static_deps, required: true)
zlib_dep = dependency('zlib', method: 'pkg-config', static: is_static_deps, required: true)
pcap_dep = dependency('libpcap', method: 'pkg-config', static: is_static_deps, required: true)
numa_dep = dependency('numa', method: 'pkg-config', static: is_static_deps, required: true)

common_deps = [
  dpdk_dep,
  glog_dep,
  gflags_dep,
  protobuf_dep,
  grpcpp_dep,
  libunwind_dep,
  zlib_dep,
  pcap_dep,
  numa_dep,
]

# gRPC server reflection (grpcurl and similar tools work without the protos,
# D-026). Shipped with grpc++ but without a pkg-config file; optional.
grpc_reflection_lib = cpp.find_library('grpc++_reflection', required: false)
grpc_reflection_deps = []
if grpc_reflection_lib.found()
  grpc_reflection_deps = [declare_dependency(
    dependencies: grpc_reflection_lib,
    compile_args: ['-DBESS_GRPC_REFLECTION'],
  )]
  common_deps += grpc_reflection_deps
endif

gtest_dep = dependency('gtest', method: 'pkg-config', required: true)
benchmark_dep = dependency('benchmark', method: 'pkg-config', required: true)
benchmark_main_dep = dependency('benchmark_main', method: 'pkg-config',
                                required: false)
if not benchmark_main_dep.found()
  benchmark_main_dep = declare_dependency(
    dependencies: [
      benchmark_dep,
      cpp.find_library('benchmark_main', required: true),
    ],
  )
endif

protoc = find_program('protoc', required: true)
grpc_cpp_plugin = find_program('grpc_cpp_plugin', required: true)
grpc_python_plugin = find_program('grpc_python_plugin', required: true)

version_h = custom_target(
  'version.h',
  output: 'version.h',
  command: [python, files('tools/write_version.py'), '@OUTPUT@'],
  build_always_stale: true,
)

pkg = import('pkgconfig')
pkg.generate(
  name: 'bess-dev',
  filebase: 'bess-dev',
  description: 'BESS dataplane plugin and module development headers',
  version: meson.project_version(),
  subdirs: ['bess', 'bess/core'],
  extra_cflags: ['-D_GNU_SOURCE', '-DGLOG_USE_GLOG_EXPORT', '-include', 'cinttypes'],
  requires: ['libdpdk', 'libglog', 'protobuf', 'grpc++'],
)

subdir('protobuf')
subdir('pybess')
subdir('core')
if get_option('build_sample_plugin')
  subdir('sample_plugin')
endif
install_subdir(
  'bessctl',
  install_dir: bess_data_dir,
  exclude_directories: ['__pycache__'],
)
install_subdir('protobuf', install_dir: bess_data_dir)
