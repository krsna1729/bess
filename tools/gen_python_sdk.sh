#!/usr/bin/env bash
# Regenerates the Python SDK's committed control_v2 code (D-146).
#
# The output is checked in, and CI regenerates it and diffs, so the committed
# code cannot drift from the protos - the same contract tools/gen_go_sdk.sh
# keeps for sdk/go/controlv2.
#
# Needs only grpcio-tools: its bundled protoc generates the messages, the
# type stubs and the service stubs in one invocation, so the committed code is
# reproducible from a single pinned toolchain - the same rule
# tools/gen_go_sdk.sh keeps for sdk/go/controlv2.
#
# The pin is not enough on its own. protoc writes its own protobuf version into
# control_v2_pb2.py, and that version comes from the protobuf the packager
# built grpcio-tools against, not from the grpcio-tools release: the PyPI wheel
# of 1.83.1 stamps 7.35.1 while Arch's build of 1.83.1 stamps 7.36.1, so CI and
# a workstation disagreed on two lines of generated code. That stamp is
# normalized below to the floor sdk/python/pyproject.toml declares, so it is a
# function of the protos and that floor rather than of the build.
#
# Only the stamp is normalized: every other byte is still whatever the
# packager's protoc emitted. The two builds agree on those bytes today (CI's
# diff was the stamp alone, with the descriptor bytes and the .pyi matching),
# but that is a property of this proto and these builds, and the lane's diff is
# what keeps it true.
#
# A stamp older than the floor but of the same major is rewritten up, which is
# the safe direction: a newer runtime supports older gencode, and the import
# check only gets stricter. A newer stamp, or one from another major, stops the
# script: rewriting those would claim a protobuf the protoc did not target, and
# a person has to decide whether the floor or the pin moves.
#
# The gRPC version the stubs carry (GRPC_GENERATED_VERSION) is fixed by the
# grpcio-tools release rather than by a linked library, so every build of the
# pin agrees on it and it needs no normalization.
set -euo pipefail

# The pin, not just the tool: a different grpcio-tools emits different bytes,
# and the CI lane's regenerate-and-diff would then fail for the wrong reason.
want=1.83.1
have=$(python3 -c 'import importlib.metadata as m; print(m.version("grpcio-tools"))')
[ "$have" = "$want" ] || { echo "need grpcio-tools==$want, have $have" >&2; exit 1; }

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/sdk/python/bess/controlv2"
proto="$root/protobuf/control_v2.proto"

# The protobuf version the generated code demands at import: the SDK's declared
# runtime floor, not the generator's own version, so the stamp is the same
# whatever build of the pin ran. pyproject.toml has to declare the same floor,
# and the check for that is in the block below, next to the other guards.
stamp=7.36.1

python3 -m grpc_tools.protoc --proto_path="$root/protobuf" \
  --python_out="$out" --pyi_out="$out" --grpc_python_out="$out" "$proto"

# The only two edits made to generated output, both here in the generator so
# that regenerating produces the same bytes: the relative import (protoc's gRPC
# plugin writes a top-level `import control_v2_pb2 as ...`, which cannot
# resolve inside a package) and the protobuf version stamp (see the header).
# Each edit must match exactly once: a change in protoc's output shape has to
# stop the generator, not silently leave a line unnormalized.
python3 - "$out" "$stamp" "$root/sdk/python/pyproject.toml" <<'PY'
import pathlib
import re
import sys

out = pathlib.Path(sys.argv[1])
stamp = sys.argv[2]
pyproject = pathlib.Path(sys.argv[3])
major, minor, patch = stamp.split(".")


def sub_once(path, pattern, repl, what):
    text = path.read_text()
    fixed, count = re.subn(pattern, repl, text, flags=re.M)
    if count != 1:
        raise SystemExit(f"{what}: expected one match in {path.name}, found {count}")
    path.write_text(fixed)
    return text


sub_once(
    out / "control_v2_pb2_grpc.py",
    r"^import (\w+_pb2) as ",
    r"from . import \1 as ",
    "the top-level pb2 import",
)

gencode = out / "control_v2_pb2.py"
emitted = re.search(r"^# Protobuf Python Version: (.*)$", gencode.read_text(), flags=re.M)
if not emitted:
    raise SystemExit("control_v2_pb2.py carries no protobuf version comment: the generator changed")


def version(text):
    try:
        return tuple(int(part) for part in text.split("."))
    except ValueError:
        raise SystemExit(f"unparseable protobuf version stamp: {text!r}")


# The floor the SDK declares and the stamp this script writes have to be the
# same number; a comment mentioning it does not count, so match the dependency
# line itself.
declared = re.search(r'^[ \t]*"protobuf>=([^"]+)",[ \t]*$', pyproject.read_text(), flags=re.M)
if not declared:
    raise SystemExit("sdk/python/pyproject.toml declares no protobuf>= dependency line")
if declared.group(1) != stamp:
    raise SystemExit(
        f"sdk/python/pyproject.toml declares protobuf>={declared.group(1)}, "
        f"this script's stamp is {stamp}"
    )

# Rewriting the stamp down would let a runtime import code its protoc built for
# a newer one, and rewriting it across majors would claim a protobuf the protoc
# did not target; both stop the script, because either is a decision about the
# floor or the pin rather than a normalization.
emitted_version = version(emitted.group(1))
floor = version(stamp)
if emitted_version > floor:
    raise SystemExit(
        f"protoc stamps {emitted.group(1)}, newer than the SDK's floor {stamp}: "
        "raise the floor in sdk/python/pyproject.toml and this script's stamp, "
        "or pin an older grpcio-tools"
    )
if emitted_version[0] != floor[0]:
    raise SystemExit(
        f"protoc stamps {emitted.group(1)}, a different protobuf major than the SDK's floor "
        f"{stamp}: the gencode and the floor have to be the same major"
    )

sub_once(
    gencode,
    r"^# Protobuf Python Version: .*$",
    f"# Protobuf Python Version: {stamp}",
    "the protobuf version comment",
)
sub_once(
    gencode,
    r"(_runtime_version\.ValidateProtobufRuntimeVersion\(\s*"
    r"_runtime_version\.Domain\.PUBLIC,\s*)"
    r"\d+(\s*,\s*)\d+(\s*,\s*)\d+",
    lambda m: f"{m.group(1)}{major}{m.group(2)}{minor}{m.group(3)}{patch}",
    "the runtime version check",
)

# Say so when it fires: on the PyPI wheel's output it does, on Arch's it does
# not, and a silent normalization is the thing that would hide a real change.
if emitted.group(1) != stamp:
    print(f"normalized the protobuf stamp {emitted.group(1)} -> {stamp}")
PY

echo "regenerated $out"
