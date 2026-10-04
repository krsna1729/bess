#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Regenerates the Go SDK's protobuf code (sdk/go/controlv2) from
# protobuf/control_v2.proto, and tidies the module, in a pinned container so
# the output is the same on every machine (M27). CI runs it and fails if the
# committed code differs.
#
#   tools/gen_go_sdk.sh            (DOCKER="sudo docker" where docker needs it)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOCKER=${DOCKER:-docker}
IMAGE=golang:1.25
PROTOC_GEN_GO=v1.36.10
PROTOC_GEN_GO_GRPC=v1.5.1
PKG=github.com/krsna1729/bess/sdk/go/controlv2

$DOCKER run --rm -v "$ROOT:/src" -w /src -e HOME=/tmp -e GOFLAGS=-mod=mod "$IMAGE" bash -euo pipefail -c "
  apt-get -qq update >/dev/null && apt-get -qq install -y protobuf-compiler >/dev/null
  go install google.golang.org/protobuf/cmd/protoc-gen-go@$PROTOC_GEN_GO
  go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@$PROTOC_GEN_GO_GRPC
  export PATH=\$PATH:/go/bin
  rm -f sdk/go/controlv2/*.pb.go
  protoc -I protobuf -I /usr/include \
    --go_out=sdk/go/controlv2 --go_opt=paths=source_relative --go_opt=Mcontrol_v2.proto=$PKG \
    --go-grpc_out=sdk/go/controlv2 --go-grpc_opt=paths=source_relative \
    --go-grpc_opt=Mcontrol_v2.proto=$PKG \
    control_v2.proto
  # The protoc version line differs between hosts and says nothing about the code.
  sed -i '/^\/\/ \tprotoc /d; /^\/\/ - protoc /d' sdk/go/controlv2/*.pb.go
  cd sdk/go && go mod tidy && gofmt -l . | (! grep .) && go vet ./... 
  chown -R $(id -u):$(id -g) /src/sdk/go
"
