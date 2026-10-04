project(
  'bess',
  'cpp',
  version: '0.1.0',
  meson_version: '>=1.2.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=0',
    'buildtype=release',
    # Release builds define NDEBUG: no assert()/DCHECK and, since Meson 1.4, no
    # -D_GLIBCXX_ASSERTIONS (libstdc++ bounds checks), which cost CuckooMap
    # lookups 8-26%. debug/debugoptimized builds (the gating CI lanes, tests)
    # keep both; glog CHECK() stays on everywhere (D-061).
    'b_ndebug=if-release',
  ],
)
if meson.project_source_root() == meson.project_build_root()
  error('In-source builds are disallowed. Please use a separate build directory, e.g. "meson setup build-meson".')
endif


cpp = meson.get_compiler('cpp')

# protobuf's headers switch generated messages to a different global layout
# under AddressSanitizer; the system libprotobuf is not built that way, so
# descriptors come back null (Any::PackFrom crashes in TypeId::name). Every
# ASan build therefore compiles with the opt-out (M22, D-072).
if get_option('b_sanitize').contains('address')
  add_project_arguments('-DPROTOBUF_MESSAGE_GLOBALS_TEMPORARY_OPTOUT', language: 'cpp')
endif
python = import('python').find_installation('python3')
fs = import('fs')

# The ISA BESS (and every plugin built from bess-dev) is compiled for comes
# from these two options alone (M21). `cpu` is passed as -march=<cpu> on both
# supported architectures: native, or a named baseline (x86_64: x86-64-v3, the
# CI floor; aarch64: armv8.2-a, the CI floor); '' passes no flag (the
# compiler's default ISA). `arch_generic` makes core/arch/ take its portable
# paths so an x86 build compiles and tests what other architectures run.
cpu_family = host_machine.cpu_family()
if cpu_family not in ['x86_64', 'aarch64']
  warning('BESS is built and tested on x86_64 and aarch64; @0@ is unsupported'.format(cpu_family))
endif
bess_isa_args = []
cpu = get_option('cpu')
if cpu != ''
  if not cpp.has_argument('-march=' + cpu)
    error(('cpu=@0@: @1@ rejects -march=@0@ on @2@. Use native, a baseline of ' +
           'this architecture (x86_64: x86-64-v3; aarch64: armv8.2-a), or ' +
           'cpu= (empty) for the compiler default.').format(cpu, cpp.get_id(), cpu_family))
  endif
  bess_isa_args += '-march=' + cpu
endif
if get_option('arch_generic')
  bess_isa_args += '-DBESS_ARCH_GENERIC'
endif
add_project_arguments(bess_isa_args, language: 'cpp')

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

# libdpdk's cflags carry the machine flags DPDK itself was built with (x86:
# -march=<cpu_instruction_set> -mrtm, sometimes -msse4; arm64: -march=armv8-a+crc
# or -mcpu=<core>, -moutline-atomics). BESS's ISA is the cpu option alone, so
# every -m flag is dropped. Kept: include paths, the forced rte_config.h
# include and defines. Any other kind of flag stops the configure, so a new
# DPDK flag is reviewed rather than silently inherited. (DPDK's libraries
# still run only on CPUs that have the ISA DPDK was built for; the bootstrap
# builds it for the same floor, tools/bootstrap_dpdk.py.)
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
# The same flags for bess-dev.pc, with an option and its operand joined
# (-includerte_config.h): pkg.generate drops a repeated '-include' token.
dpdk_pc_cflags = []
dpdk_dropped_args = []
dpdk_flag_with_operand = ''
foreach arg : run_command(
  pkg_config, '--cflags', 'libdpdk',
  env: pkg_config_env,
  check: true,
).stdout().strip().split()
  if dpdk_flag_with_operand != ''
    dpdk_compile_args += [dpdk_flag_with_operand, arg]
    dpdk_pc_cflags += dpdk_flag_with_operand + arg
    dpdk_flag_with_operand = ''
  elif arg in ['-include', '-isystem', '-idirafter', '-iquote']
    dpdk_flag_with_operand = arg
  elif arg.startswith('-I') or arg.startswith('-D') or arg.startswith('-U') or arg.startswith('-isystem') or arg == '-pthread'
    dpdk_compile_args += arg
    dpdk_pc_cflags += arg
  elif arg.startswith('-m')
    dpdk_dropped_args += arg
  else
    error(('libdpdk cflag "@0@" is neither an include path, a define nor a ' +
           'machine flag. Decide whether BESS may inherit it (meson.build, ' +
           'DPDK cflag filter).').format(arg))
  endif
endforeach
if dpdk_flag_with_operand != ''
  error('libdpdk cflags end with "@0@" and no operand'.format(dpdk_flag_with_operand))
endif
# The guarantee itself, independent of how the filter above is written: no
# DPDK machine flag reaches BESS's compile or a plugin's.
foreach arg : dpdk_compile_args + dpdk_pc_cflags
  if arg.startswith('-m')
    error('DPDK machine flag @0@ would leak into BESS or bess-dev.pc'.format(arg))
  endif
endforeach
message('libdpdk machine flags dropped: @0@; BESS compiles with: @1@'.format(
  dpdk_dropped_args.length() > 0 ? ' '.join(dpdk_dropped_args) : '(none)',
  bess_isa_args.length() > 0 ? ' '.join(bess_isa_args) : '(the compiler default ISA)'))

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
# gflags must match glog's shared/static state to avoid double registration in gflags,
# but must be static in a fully_static build
gflags_dep = dependency('gflags', method: 'pkg-config', static: (static_binary_opt == 'fully_static'), required: true)
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

# Decision D-041: plugin compile dependencies exclude daemon-only gRPC.
# Plugins compile for bessd's ISA (M21): libdpdk is not a `requires`, because
# pkg-config would emit DPDK's own -march/-mrtm after BESS's flags and the
# last -march wins. Instead the package carries BESS's ISA flags, the DPDK
# cflags BESS itself compiles with (filtered above) and DPDK's shared-library
# link flags. The DPDK prefix is therefore fixed at BESS's build time, as it
# already was inside libdpdk.pc.
dpdk_plugin_libs = run_command(
  pkg_config, '--libs', 'libdpdk',
  env: pkg_config_env,
  check: true,
).stdout().strip().split()
pkg = import('pkgconfig')
pkg.generate(
  name: 'bess-dev',
  filebase: 'bess-dev',
  description: 'BESS dataplane plugin and module development headers',
  version: meson.project_version(),
  subdirs: ['bess', 'bess/core'],
  extra_cflags: bess_isa_args + dpdk_pc_cflags +
                ['-D_GNU_SOURCE', '-DGLOG_USE_GLOG_EXPORT', '-include', 'cinttypes'],
  libraries: dpdk_plugin_libs,
  requires: ['libglog', 'protobuf'],
)

subdir('protobuf')
subdir('pybess')
subdir('core')

# What this build is made of (M26): version, commit, compilers, options, the
# DPDK pin and every dependency's version, and an SPDX SBOM from the same
# facts. Regenerated on every build (cheap), installed with bessd.
custom_target(
  'build_info',
  output: ['build-info.json', 'bess.spdx.json'],
  command: [python, files('tools/build_info.py'),
            '--build-dir', meson.project_build_root(),
            '--source-dir', meson.project_source_root(),
            '--info', '@OUTPUT0@', '--sbom', '@OUTPUT1@'],
  build_by_default: true,
  build_always_stale: true,
  install: true,
  install_dir: [join_paths(get_option('datadir'), 'bess'),
                join_paths(get_option('datadir'), 'doc', 'bess')],
)
if get_option('build_sample_plugin')
  subdir('sample_plugin')
endif
install_subdir(
  'bessctl',
  install_dir: bess_data_dir,
  exclude_directories: ['__pycache__'],
)
install_subdir('protobuf', install_dir: bess_data_dir)
