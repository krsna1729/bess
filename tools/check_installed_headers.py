#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS Curated Public Headers Verifier (Milestone M2).

Verifies the installed 'bess-dev' C++ headers tree:
1. Ensures internal headers (runtime internals, control-plane RPC, drivers) are NOT installed.
2. Ensures curated public headers (module.h, framework/plugin.h, dataplane/scope.h, etc.) ARE installed.
3. Performs a standalone C++ compilation of a plugin against the installed include tree.
4. Performs a negative compilation test proving that attempts to include internal headers fail.

Usage:
  tools/check_installed_headers.py --include-dir /path/to/include/bess/core
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

INTERNAL_FORBIDDEN = [
    "bessctl.h",
    "bessd.h",
    "debug.h",
    "dpdk.h",
    "module_graph.h",
    "resume_hook.h",
    "scheduler.h",
    "shared_obj.h",
    "traffic_class.h",
    "runtime",
    "control",
    "drivers",
    "gate_hooks",
    "resume_hooks",
    "utils/bpf_program.h",
    "utils/cuckoo_map.h",
    "dataplane/transaction_engine.h",
    "dataplane/transaction_engine_test.cc",
    "classifier/concurrent_exact.h",
    "classifier/concurrent_masked.h",
    "flow/shared_flow_table.h",
    "route/router_transaction_test.cc",
]

PUBLIC_REQUIRED = [
    "commands.h",
    "event.h",
    "gate.h",
    "message.h",
    "metadata.h",
    "module.h",
    "packet.h",
    "packet_checksum.h",
    "packet_cursor.h",
    "packet_handle.h",
    "packet_mutation.h",
    "packet_pool.h",
    "packet_reshape.h",
    "packet_tx_checksum.h",
    "pktbatch.h",
    "port.h",
    "snbuf_layout.h",
    "task.h",
    "worker.h",
    "framework/instance_registry.h",
    "framework/module_init_context.h",
    "framework/plugin.h",
    "utils/common.h",
    "utils/copy.h",
    "utils/endian.h",
    "utils/ether.h",
    "utils/extended_priority_queue.h",
    "utils/inline_function.h",
    "utils/random.h",
    "utils/time.h",
    "dataplane/action_id.h",
    "dataplane/batch_stages.h",
    "dataplane/batch_tuning.h",
    "dataplane/generation_handle.h",
    "dataplane/interface_id.h",
    "dataplane/object_table.h",
    "dataplane/resource.h",
    "dataplane/scope.h",
    "dataplane/slot_resource.h",
    "dataplane/slot_table.h",
    "dataplane/strong_id.h",
    "dataplane/worker_id.h",
    "classifier/byte_key.h",
    "classifier/classifier.h",
    "classifier/range_backend.h",
    "flow/flow_index.h",
    "flow/flow_key.h",
    "flow/flow_observer.h",
    "flow/flow_storage.h",
    "flow/flow_types.h",
    "flow/owner.h",
    "flow/worker_flow_table.h",
    "meter/meter.h",
    "meter/meter_set.h",
    "route/next_hop_id.h",
    "route/route_domain.h",
    "route/route_table.h",
    "route/router.h",
    "rcu/rcu_domain.h",
    "rcu/rcu_ptr.h",
    "stats/counter_set.h",
    "stats/worker_histogram.h",
    "stats/worker_local.h",
    "stats/worker_slots.h",
]


def verify_headers(include_dir: Path):
    print(f"Verifying installed headers in: {include_dir}")
    if not include_dir.is_dir():
        raise RuntimeError(f"Installed header directory does not exist: {include_dir}")

    for internal in INTERNAL_FORBIDDEN:
        path = include_dir / internal
        if path.exists():
            raise RuntimeError(
                f"Internal header or subtree '{internal}' was installed: {path}"
            )
    print("  OK: private headers and subtrees are absent.")

    for public in PUBLIC_REQUIRED:
        if not (include_dir / public).is_file():
            raise RuntimeError(f"Required public header is missing: {public}")
    print(f"  OK: all {len(PUBLIC_REQUIRED)} curated headers are installed.")

    pkg_config = os.environ.get("PKG_CONFIG", "pkg-config")
    requirements = subprocess.run(
        [pkg_config, "--print-requires", "bess-dev"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    if any(line.split()[0] == "grpc++" for line in requirements if line.strip()):
        raise RuntimeError("bess-dev must not require grpc++ for module plugins")

    pkg_cflags = subprocess.run(
        [pkg_config, "--cflags", "bess-dev"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    pkg_includedir = Path(
        subprocess.run(
            [pkg_config, "--variable=includedir", "bess-dev"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    # The stage's BESS include root is explicit below. Do not let a stale
    # system-installed bess-dev tree satisfy missing headers.
    bess_include_flags = {
        f"-I{pkg_includedir / 'bess'}",
        f"-I{pkg_includedir / 'bess' / 'core'}",
    }
    pkg_flags = [
        flag for flag in shlex.split(pkg_cflags) if flag not in bess_include_flags
    ]
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler:
        raise RuntimeError("CXX must name a C++ compiler")

    with tempfile.TemporaryDirectory(prefix="bess-header-test-") as tempdir:
        test_cpp = Path(tempdir) / "test_plugin.cc"
        includes = "\n".join(f'#include "{header}"' for header in PUBLIC_REQUIRED)
        test_cpp.write_text(
            includes
            + """

BESS_PLUGIN("test_plugin", "1.0.0");

class TestModule final : public Module {
 public:
  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }
  void ProcessBatch(Context *, bess::PacketBatch *) override {}
};

ADD_MODULE(TestModule, "test_module", "installed-header conformance module")
"""
        )

        cmd = [
            *compiler,
            "-std=c++23",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fPIC",
            f"-I{include_dir.parent.parent}",
            f"-I{include_dir.parent}",
            f"-I{include_dir}",
            *pkg_flags,
            "-c",
            str(test_cpp),
            "-o",
            os.devnull,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"Standalone plugin compilation failed:\n{result.stderr}"
            )
        print("  OK: plugin and curated API headers compile from installed artifacts.")

        bad_cpp = Path(tempdir) / "bad_plugin.cc"
        bad_cpp.write_text(
            '#include "module.h"\n#include "runtime/runtime_state.h"\n'
        )
        bad_cmd = list(cmd)
        bad_cmd[bad_cmd.index(str(test_cpp))] = str(bad_cpp)
        bad_result = subprocess.run(bad_cmd, capture_output=True, text=True)
        if (
            bad_result.returncode == 0
            or "runtime/runtime_state.h" not in bad_result.stderr
        ):
            raise RuntimeError(
                "Negative test did not fail specifically on the forbidden "
                f"runtime header:\n{bad_result.stderr}"
            )
        print("  OK: intentionally private runtime include is rejected.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--include-dir",
        required=True,
        type=Path,
        help="Path to installed include/bess/core directory",
    )
    args = parser.parse_args()

    try:
        verify_headers(args.include_dir)
        print("ALL HEADER VERIFICATION CHECKS PASSED.")
        return 0
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
