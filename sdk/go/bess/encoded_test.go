// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"errors"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
	"google.golang.org/protobuf/types/known/anypb"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// An encoded operation is its message counterpart as the daemon reads it: the
// same resource, erase flag and type URL, and bytes that parse to the same
// message. (A dynamic message marshals its fields in no fixed order, so the
// bytes themselves may differ.) That is what lets a controller switch between
// them.
func TestEncodedOperationsAreTheirMessagesOnTheWire(t *testing.T) {
	file, err := protodesc.NewFile(benchSchema(), nil)
	if err != nil {
		t.Fatal(err)
	}
	key, value := benchMessage(file, "RuleKey", 3), benchMessage(file, "RuleValue", 3)
	k, err := proto.Marshal(key)
	if err != nil {
		t.Fatal(err)
	}
	v, err := proto.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	rules := Resource{Name: "bench/rules", Kind: pb.TransactionResource_KIND_MAP,
		KeyType: "bench.RuleKey", ValueType: "bench.RuleValue"}
	settings := Resource{Name: "bench/settings", Kind: pb.TransactionResource_KIND_SINGLETON,
		KeyType: "google.protobuf.Empty", ValueType: "bench.RuleValue"}

	byMessage, byBytes := (&Client{}).Transaction(), (&Client{}).Transaction()
	for _, err := range []error{
		byMessage.Upsert(rules, key, value), byBytes.UpsertEncoded(rules, k, v),
		byMessage.Set(settings, value), byBytes.SetEncoded(settings, v),
		byMessage.Erase(rules, key), byBytes.EraseEncoded(rules, k),
	} {
		if err != nil {
			t.Fatal(err)
		}
	}
	if len(byMessage.ops) != len(byBytes.ops) {
		t.Fatalf("%d ops from messages, %d from bytes", len(byMessage.ops), len(byBytes.ops))
	}
	read := func(a *anypb.Any) proto.Message {
		if a == nil {
			return nil
		}
		m := dynamicpb.NewMessage(file.Messages().ByName(protoreflect.Name(a.GetTypeUrl()[len("type.googleapis.com/bench."):])))
		if err := proto.Unmarshal(a.GetValue(), m); err != nil {
			t.Fatal(err)
		}
		return m
	}
	for i, m := range byMessage.ops {
		b := byBytes.ops[i]
		if m.GetResource() != b.GetResource() || m.GetErase() != b.GetErase() ||
			m.GetKey().GetTypeUrl() != b.GetKey().GetTypeUrl() || m.GetValue().GetTypeUrl() != b.GetValue().GetTypeUrl() ||
			!proto.Equal(read(m.GetKey()), read(b.GetKey())) || !proto.Equal(read(m.GetValue()), read(b.GetValue())) {
			t.Errorf("op %d: from messages %v, from bytes %v", i, m, b)
		}
	}
}

// An encoded operation is refused where its message counterpart is: a map op
// on a singleton, a Set on a map, a resource of another daemon epoch.
func TestEncodedOperationsRefuseWhatTheirMessagesRefuse(t *testing.T) {
	rules := Resource{Name: "r", Kind: pb.TransactionResource_KIND_MAP, KeyType: "k", ValueType: "v", DaemonEpoch: 1}
	settings := Resource{Name: "s", Kind: pb.TransactionResource_KIND_SINGLETON, KeyType: "google.protobuf.Empty", ValueType: "v", DaemonEpoch: 1}
	tx := (&Client{}).Transaction()
	for name, err := range map[string]error{
		"upsert into a singleton": tx.UpsertEncoded(settings, nil, nil),
		"erase from a singleton":  tx.EraseEncoded(settings, nil),
		"set a map":               tx.SetEncoded(rules, nil),
	} {
		var invalid *InvalidRequestError
		if !errors.As(err, &invalid) {
			t.Errorf("%s: answered %v, want an InvalidRequestError", name, err)
		}
	}
	if err := tx.UpsertEncoded(rules, nil, nil); err != nil {
		t.Fatal(err)
	}
	other := rules
	other.DaemonEpoch = 2
	var stale *StaleResourceError
	if err := tx.UpsertEncoded(other, nil, nil); !errors.As(err, &stale) {
		t.Errorf("a resource of another epoch: answered %v, want a StaleResourceError", err)
	}
	if len(tx.ops) != 1 {
		t.Errorf("the transaction holds %d operations, want the 1 accepted", len(tx.ops))
	}
}

// An encoded upsert costs one allocation, the operation: the bytes go out as
// the caller gave them and the type URLs are built once per type.
func TestAnEncodedUpsertIsOneAllocation(t *testing.T) {
	rules := Resource{Name: "bench/rules", Kind: pb.TransactionResource_KIND_MAP,
		KeyType: "bench.RuleKey", ValueType: "bench.RuleValue"}
	k, v := Encoded{1, 2, 3}, Encoded{4, 5}
	tx := (&Client{}).Transaction()
	allocs := testing.AllocsPerRun(100, func() {
		tx.ops = tx.ops[:0]
		if err := tx.UpsertEncoded(rules, k, v); err != nil {
			t.Fatal(err)
		}
	})
	if allocs > 1 {
		t.Errorf("an encoded upsert made %.0f allocations, want 1", allocs)
	}
}
