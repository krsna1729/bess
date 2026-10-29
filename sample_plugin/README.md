## Sample plugin

This is just a sample of a way to add plugins to BESS.

To use this particular sample plugin, configure BESS with Meson and build it
from the top level:

    tools/bootstrap_dpdk.py --af-xdp auto
    export PKG_CONFIG_PATH="$(tools/bootstrap_dpdk.py --print-pkg-config-path):${PKG_CONFIG_PATH}"
    meson setup build -Dbuild_sample_plugin=true
    meson compile -C build -j4

The shared module is written to `build/sample_plugin/libsequential_update.so`
and installed under `lib/bess/modules`.  The plugin's protobuf sources are
generated in the build tree; no generated files are written into this source
directory.

`sequential_update.yaml` is a pipeline that uses it:

    bessctl plugin import "$PWD/build/sample_plugin/libsequential_update.so"
    bessctl apply sample_plugin/sequential_update.yaml && bessctl resume

### UsageReport memory

`usage_report` admits its argument-sized storage through
`init_context().memory()` before allocating it (D-198): `"counters"` is a
`KeyedCounters(keys, 2, workers)` and `"quota_slots"` is a DPDK array of one
relaxed atomic quota per key. The latter stays by value with a memory lease,
so the packet path still reads the array through one pointer.

Threshold watches are `"quotas"`. Each new watch admits its estimated glibc
storage through the table's growth hook. A quota update stages a complete new
watch table and its lease before publication; apply only stores atomics and
moves the staged table and lease. A refusal therefore leaves the live quotas
unchanged and reaches the control boundary as `RESOURCE_EXHAUSTED`.
