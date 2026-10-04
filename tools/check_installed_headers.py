#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""BESS Curated Public Headers Verifier (Milestone M2, D-058).

Verifies the installed 'bess-dev' C++ headers tree against the classification
table in tools/api_classes.json (every installed header is `public` or
`experimental`; internal headers are never installed):
1. The installed set equals the table: a header installed but not classified,
   and a classified header that is not installed, both fail. Named internal
   files (runtime, control, drivers, transaction engine, `*.grpc.pb.h`, the
   control and test protocols, `.pb.cc`) fail with their own message.
2. A public header includes only public headers (and the generated ones);
   every quoted include of an installed header resolves inside the installed
   tree.
3. The generated headers installed are exactly the closure the public headers
   need (tools/public_proto_closure.py).
4. A plugin that includes every installed header compiles against the installed
   tree alone, and a plugin that includes an internal header fails to.

Usage:
  tools/check_installed_headers.py --include-dir /path/to/include/bess/core
  tools/check_installed_headers.py --self-test
"""

import argparse
import json
import os
from pathlib import Path
import posixpath
import shlex
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import public_proto_closure  # noqa: E402

MANIFEST = Path(__file__).resolve().with_name("api_classes.json")
CLASSES = ("public", "experimental")

# Files and directories that must never be installed. The exact-set check below
# already refuses anything unlisted; these give the common mistakes their own
# message and keep a table edit from listing one of them.
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
    "classifier/concurrent_exact.h",
    "classifier/concurrent_masked.h",
    # Generated protocol the public headers do not include.
    "pb/service.pb.h",
    "pb/control_v2.pb.h",
    "pb/test_msg.pb.h",
    "pb/ingress_bench.pb.h",
]

# Never installed whatever the table says: gRPC stubs, generated and test sources.
FORBIDDEN_SUFFIXES = (".grpc.pb.h", ".grpc.pb.cc", ".pb.cc", "_test.cc", "_bench.cc")


def is_internal(name):
    return name.endswith(FORBIDDEN_SUFFIXES) or any(
        name == item or name.startswith(item + "/") for item in INTERNAL_FORBIDDEN
    )


def load_manifest(path=MANIFEST):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return data["headers"], list(data["generated"]["headers"])


def table_problems(installed, headers, generated):
    """Differences between the installed file set and the classification table."""
    problems = []
    for name, cls in sorted(headers.items()):
        if cls not in CLASSES:
            problems.append(f"{name} has class {cls!r}; the classes are {CLASSES}")
    for name in sorted(set(headers) | set(generated)):
        if is_internal(name):
            problems.append(f"api_classes.json lists an internal file: {name}")
    for name in sorted(installed):
        if is_internal(name):
            problems.append(f"internal file was installed: {name}")
    listed = set(headers) | set(generated)
    for name in sorted(installed - listed):
        if not is_internal(name):
            problems.append(f"installed but not classified in api_classes.json: {name}")
    for name in sorted(listed - installed):
        problems.append(f"classified in api_classes.json but not installed: {name}")
    return problems


def resolve(tree, header, inc):
    """The installed file `inc` names from `header`, as the compiler finds it."""
    for candidate in (
        posixpath.normpath(posixpath.join(posixpath.dirname(header), inc)),
        posixpath.normpath(inc),
    ):
        if candidate in tree:
            return candidate
    return None


def include_problems(tree, headers):
    """A public header may include only public headers; every include resolves.

    `tree` maps each installed file to the quoted includes it contains.
    """
    problems = []
    for name in sorted(headers):
        for inc in tree.get(name, []):
            target = resolve(tree, name, inc)
            if target is None:
                problems.append(f'{name} includes "{inc}", which is not installed')
            elif headers[name] == "public" and headers.get(target) == "experimental":
                problems.append(
                    f"public {name} includes experimental {target}: "
                    "promote it or stop including it"
                )
    return problems


def machine_flag_problems(cflags, march=None):
    """Plugins compile for bessd's ISA: at most one -march (BESS's cpu option)
    and no other machine flag, such as DPDK's own -march or -mrtm (M21).
    `march`, when given, is bessd's cpu option: then exactly that -march."""
    machine = [flag for flag in cflags if flag.startswith("-m")]
    if len(machine) > 1 or any(not flag.startswith("-march=") for flag in machine):
        return [f"bess-dev cflags carry machine flags {machine}; only bessd's "
                "-march may reach a plugin build"]
    if march is not None and machine != ([f"-march={march}"] if march else []):
        return [f"bess-dev cflags carry {machine or 'no -march'}; bessd was built "
                f"with cpu={march!r}"]
    return []


def verify_headers(include_dir: Path, march=None):
    print(f"Verifying installed headers in: {include_dir}")
    if not include_dir.is_dir():
        raise RuntimeError(f"Installed header directory does not exist: {include_dir}")

    headers, generated = load_manifest()
    installed = {
        p.relative_to(include_dir).as_posix()
        for p in include_dir.rglob("*")
        if p.is_file()
    }
    problems = table_problems(installed, headers, generated)
    if problems:
        raise RuntimeError(
            "installed headers differ from tools/api_classes.json:\n  "
            + "\n  ".join(problems)
        )
    counts = {c: sum(1 for v in headers.values() if v == c) for c in CLASSES}
    print(
        f"  OK: the installed set is exactly the table ({counts['public']} public, "
        f"{counts['experimental']} experimental, {len(generated)} generated); "
        "no internal file is installed."
    )

    tree = {
        name: public_proto_closure.quoted_includes(include_dir / name)
        for name in installed
        if name.endswith(".h")
    }
    problems = include_problems(tree, headers)
    if problems:
        raise RuntimeError("include graph of installed headers:\n  " + "\n  ".join(problems))
    print("  OK: public headers include only public headers; every include resolves.")

    needed = public_proto_closure.closure(include_dir, headers, include_dir / "pb")
    if needed != set(generated):
        raise RuntimeError(
            f"generated headers listed {sorted(generated)} but the public headers "
            f"need {sorted(needed)}"
        )
    print(f"  OK: the {len(needed)} generated headers are exactly the closure the public headers need.")

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
    leaks = machine_flag_problems(pkg_flags, march)
    if leaks:
        raise RuntimeError("\n".join(leaks))
    isa = [flag for flag in pkg_flags if flag.startswith("-march=")]
    print(f"  OK: bess-dev's ISA flags are bessd's alone: {' '.join(isa) or 'compiler default'}")
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler:
        raise RuntimeError("CXX must name a C++ compiler")

    with tempfile.TemporaryDirectory(prefix="bess-header-test-") as tempdir:
        test_cpp = Path(tempdir) / "test_plugin.cc"
        includes = "\n".join(f'#include "{header}"' for header in sorted(headers))
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

        # Neither header is installed; the plugin must fail on exactly that one.
        for bad_header in ("runtime/runtime_state.h", "pb/service.grpc.pb.h"):
            bad_cpp = Path(tempdir) / "bad_plugin.cc"
            bad_cpp.write_text(f'#include "module.h"\n#include "{bad_header}"\n')
            bad_cmd = list(cmd)
            bad_cmd[bad_cmd.index(str(test_cpp))] = str(bad_cpp)
            bad_result = subprocess.run(bad_cmd, capture_output=True, text=True)
            if bad_result.returncode == 0 or bad_header not in bad_result.stderr:
                raise RuntimeError(
                    "Negative test did not fail specifically on the forbidden "
                    f"header {bad_header}:\n{bad_result.stderr}"
                )
            print(f"  OK: intentionally private include {bad_header} is rejected.")

        # An author cannot fetch a context for themselves: only Module's
        # constructor may call ProcessDefault() (D-057).
        ctx_cpp = Path(tempdir) / "ctx_plugin.cc"
        ctx_cpp.write_text(
            '#include "module.h"\n'
            "const bess::framework::ModuleInitContext &Grab() {\n"
            "  return bess::framework::ModuleInitContext::ProcessDefault();\n"
            "}\n"
        )
        ctx_cmd = list(cmd)
        ctx_cmd[ctx_cmd.index(str(test_cpp))] = str(ctx_cpp)
        ctx_result = subprocess.run(ctx_cmd, capture_output=True, text=True)
        if (
            ctx_result.returncode == 0
            or "ProcessDefault" not in ctx_result.stderr
            or "private" not in ctx_result.stderr
        ):
            raise RuntimeError(
                "ModuleInitContext::ProcessDefault() is reachable from "
                f"plugin code:\n{ctx_result.stderr}"
            )
        print("  OK: ModuleInitContext::ProcessDefault() is not callable by authors.")


def run_self_test():
    """The table check and the include rule catch what they claim to catch."""
    print("Running check_installed_headers self-test...")
    headers = {
        "module.h": "public",
        "utils/common.h": "public",
        "flow/flow_key.h": "experimental",
    }
    generated = ["pb/error.pb.h"]
    everything = set(headers) | set(generated)

    def problems(installed, hdrs=headers, gen=generated):
        return "\n".join(table_problems(installed, hdrs, gen))

    assert problems(everything) == "", problems(everything)
    for extra, want in [
        ("flow/flow_table.h", "installed but not classified in api_classes.json: flow/flow_table.h"),
        ("pb/service.grpc.pb.h", "internal file was installed: pb/service.grpc.pb.h"),
        ("pb/error.grpc.pb.h", "internal file was installed: pb/error.grpc.pb.h"),
        ("pb/control_v2.pb.h", "internal file was installed: pb/control_v2.pb.h"),
        ("pb/error.pb.cc", "internal file was installed: pb/error.pb.cc"),
        ("runtime/runtime_state.h", "internal file was installed: runtime/runtime_state.h"),
        ("flow/worker_flow_table_test.cc", "internal file was installed"),
    ]:
        assert want in problems(everything | {extra}), (extra, problems(everything | {extra}))
    for gone in sorted(everything):
        assert f"classified in api_classes.json but not installed: {gone}" in problems(everything - {gone}), gone
    assert "has class 'internal'" in problems(everything, {**headers, "module.h": "internal"})
    assert "lists an internal file: runtime/x.h" in problems(
        everything | {"runtime/x.h"}, {**headers, "runtime/x.h": "public"}
    )

    # The include rule: `tree` is installed file -> its quoted includes.
    base = {"module.h": [], "utils/common.h": [], "flow/flow_key.h": ["utils/common.h"]}
    assert include_problems(base, headers) == []
    got = include_problems({**base, "module.h": ["flow/flow_key.h"]}, headers)
    assert got == ["public module.h includes experimental flow/flow_key.h: promote it or stop including it"], got
    got = include_problems({**base, "module.h": ["utils/missing.h"]}, headers)
    assert got == ['module.h includes "utils/missing.h", which is not installed'], got
    # An experimental header may include public and experimental ones.
    assert include_problems({**base, "flow/flow_key.h": ["module.h", "flow/flow_key.h"]}, headers) == []
    # A sibling include resolves relative to the includer; `..` resolves too.
    sibling = {**base, "flow/owner.h": [], "flow/flow_key.h": ["owner.h", "../module.h"]}
    assert include_problems(sibling, {**headers, "flow/owner.h": "experimental"}) == []
    # A public header reaching an experimental one by a sibling spelling is caught.
    reach = {**base, "utils/common.h": ["../flow/flow_key.h"]}
    got = include_problems(reach, headers)
    assert got == ["public utils/common.h includes experimental flow/flow_key.h: promote it or stop including it"], got
    # bess-dev carries bessd's ISA and nothing from a dependency's build machine.
    assert machine_flag_problems(["-I/x", "-march=x86-64-v3", "-DBESS_ARCH_GENERIC"]) == []
    assert machine_flag_problems(["-I/x"]) == []
    for leak in (["-march=x86-64-v3", "-march=native"], ["-march=x86-64-v3", "-mrtm"],
                 ["-mcpu=neoverse-n1"], ["-march=armv8.2-a", "-moutline-atomics"]):
        assert machine_flag_problems(["-I/x", *leak]), leak
    # With bessd's cpu known, the plugin gets exactly that -march.
    assert machine_flag_problems(["-march=armv8.2-a"], "armv8.2-a") == []
    assert machine_flag_problems([], "") == []
    assert machine_flag_problems([], "x86-64-v3")
    assert machine_flag_problems(["-march=native"], "x86-64-v3")
    assert machine_flag_problems(["-march=native"], "")
    print("Self-test PASSED: table, internal-file and include rules detect every injected defect.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--include-dir",
        type=Path,
        help="Path to installed include/bess/core directory",
    )
    parser.add_argument("--self-test", action="store_true",
                        help="check the checker's own rules, no install needed")
    parser.add_argument("--march", default=None,
                        help="bessd's cpu option: bess-dev must carry exactly -march=<it>")
    args = parser.parse_args()
    if args.self_test:
        run_self_test()
        return 0
    if args.include_dir is None:
        parser.error("--include-dir is required")

    try:
        verify_headers(args.include_dir, args.march)
        print("ALL HEADER VERIFICATION CHECKS PASSED.")
        return 0
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
