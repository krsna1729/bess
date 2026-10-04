// SPDX-License-Identifier: BSD-3-Clause

//go:build live

package bess

// Against a live bessd (bessctl/module_tests/control_sdk_go.py starts it and
// writes testdata/live.json): discovery, a typed commit, a replay under the
// same request id, a conflict, and a client-side type refusal. The module's
// messages are built at run time from a descriptor set (dynamicpb), as a
// client in a language without generated types would.

import (
	"context"
	"encoding/json"
	"os"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
)

type liveConfig struct {
	Address     string `json:"address"`
	Resource    string `json:"resource"`
	Descriptors []byte `json:"descriptors"` // a FileDescriptorSet
	Key         []byte `json:"key"`         // serialized key message
	Value       []byte `json:"value"`       // serialized value message
}

func message(t *testing.T, files protoreflectFiles, name string, data []byte) proto.Message {
	t.Helper()
	d, err := files.FindDescriptorByName(protoreflect.FullName(name))
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	m := dynamicpb.NewMessage(d.(protoreflect.MessageDescriptor))
	if err := proto.Unmarshal(data, m); err != nil {
		t.Fatal(err)
	}
	return m
}

type protoreflectFiles interface {
	FindDescriptorByName(protoreflect.FullName) (protoreflect.Descriptor, error)
}

func TestLive(t *testing.T) {
	path := os.Getenv("BESS_LIVE_CONFIG")
	if path == "" {
		path = "testdata/live.json"
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var cfg liveConfig
	if err := json.Unmarshal(raw, &cfg); err != nil {
		t.Fatal(err)
	}
	set := &descriptorpb.FileDescriptorSet{}
	if err := proto.Unmarshal(cfg.Descriptors, set); err != nil {
		t.Fatal(err)
	}
	files, err := protodesc.NewFiles(set)
	if err != nil {
		t.Fatal(err)
	}

	ctx := context.Background()
	client, err := Dial(cfg.Address)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	r, err := client.Resource(ctx, cfg.Resource)
	if err != nil {
		t.Fatal(err)
	}
	key := message(t, files, r.KeyType, cfg.Key)
	value := message(t, files, r.ValueType, cfg.Value)

	tx := client.Transaction()
	if err := tx.Upsert(r, value, value); err == nil {
		t.Fatal("a value as key was accepted")
	}
	if err := tx.Upsert(r, key, value); err != nil {
		t.Fatal(err)
	}
	first, err := tx.Commit(ctx)
	if err != nil {
		t.Fatal(err)
	}

	again := client.Transaction(WithRequestID(tx.RequestID))
	if err := again.Upsert(r, key, value); err != nil {
		t.Fatal(err)
	}
	replay, err := again.Commit(ctx)
	if err != nil || !replay.Replayed || replay.Generation != first.Generation {
		t.Fatalf("replay %+v %v", replay, err)
	}

	stale := client.Transaction(WithExpectedGeneration(first.Generation - 1))
	if err := stale.Upsert(r, key, value); err != nil {
		t.Fatal(err)
	}
	if _, err := stale.Commit(ctx); err == nil {
		t.Fatal("a stale generation applied")
	} else if _, ok := err.(*ConflictError); !ok {
		t.Fatalf("got %v, want a conflict", err)
	}
}
