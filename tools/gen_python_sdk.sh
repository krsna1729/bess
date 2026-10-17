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
# tools/gen_go_sdk.sh keeps for sdk/go/controlv2. The runtime floors in
# sdk/python/pyproject.toml follow whatever that toolchain emits
# (ValidateProtobufRuntimeVersion and GRPC_GENERATED_VERSION).
set -euo pipefail

# The pin, not just the tool: a different grpcio-tools emits different bytes,
# and the CI lane's regenerate-and-diff would then fail for the wrong reason.
want=1.83.1
have=$(python3 -c 'import importlib.metadata as m; print(m.version("grpcio-tools"))')
[ "$have" = "$want" ] || { echo "need grpcio-tools==$want, have $have" >&2; exit 1; }

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/sdk/python/bess/controlv2"
proto="$root/protobuf/control_v2.proto"

python3 -m grpc_tools.protoc --proto_path="$root/protobuf" \
  --python_out="$out" --pyi_out="$out" --grpc_python_out="$out" "$proto"

# protoc's gRPC plugin writes a top-level import (`import control_v2_pb2 as ...`),
# which cannot resolve when the module lives inside a package. Rewrite it to a
# relative import. This is the only edit made to generated output, and it is
# here, in the generator, so regenerating produces the same bytes.
python3 - "$out/control_v2_pb2_grpc.py" <<'PY'
import re
import sys

path = sys.argv[1]
with open(path) as handle:
    text = handle.read()
fixed = re.sub(r"^import (\w+_pb2) as ", r"from . import \1 as ", text, flags=re.M)
if fixed == text:
    raise SystemExit(f"no top-level pb2 import found in {path}: the generator changed")
with open(path, "w") as handle:
    handle.write(fixed)
PY

echo "regenerated $out"
