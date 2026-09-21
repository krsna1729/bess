## Sample plugin

This is just a sample of a way to add plugins to BESS.

To use this particular sample plugin, configure BESS with Meson and build it
from the top level:

    tools/bootstrap_dpdk.py --af-xdp auto
    export PKG_CONFIG_PATH="$(tools/bootstrap_dpdk.py --print-pkg-config-path):${PKG_CONFIG_PATH}"
    meson setup build -Dbuild_sample_plugin=true
    meson compile -C build

The shared module is written to `build/sample_plugin/libsequential_update.so`
and installed under `lib/bess/modules`.  The plugin's protobuf sources are
generated in the build tree; no generated files are written into this source
directory.
