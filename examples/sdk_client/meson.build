project('sdk_client', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=1',
  ],
)

# Built from the installed bess-sdk package only (pkg-config bess-sdk): the
# install check (tools/ci_profile.py) builds it against the staged install.
executable('list_resources', 'list_resources.cc',
           dependencies: [dependency('bess-sdk', required: true)])
