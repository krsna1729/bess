// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"errors"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// An upsert costs the marshalling of its key and value and one allocation of
// its own, the operation: no required-field walk of a type that has no
// required field, and no type URL built per operation. A schema-driven
// controller upserts several rules for every session it installs.
func TestUpsertAllocatesItsMarshalsAndOneOperation(t *testing.T) {
	file, err := protodesc.NewFile(benchSchema(), nil)
	if err != nil {
		t.Fatal(err)
	}
	key, value := benchMessage(file, "RuleKey", 7), benchMessage(file, "RuleValue", 7)
	r := Resource{Name: "bench/rules", Kind: pb.TransactionResource_KIND_MAP,
		KeyType: "bench.RuleKey", ValueType: "bench.RuleValue"}
	o := proto.MarshalOptions{AllowPartial: true}
	marshal := testing.AllocsPerRun(100, func() {
		_, _ = o.Marshal(key)
		_, _ = o.Marshal(value)
	})
	tx := (&Client{}).Transaction()
	upsert := testing.AllocsPerRun(100, func() {
		tx.ops = tx.ops[:0]
		if err := tx.Upsert(r, key, value); err != nil {
			t.Fatal(err)
		}
	})
	if upsert > marshal+1 {
		t.Errorf("an upsert allocated %.0f times; marshalling its key and value allocates %.0f", upsert, marshal)
	}
}

// requiredFiles is a proto2 type with a required field, a proto3 type that
// holds it (once and repeated), a proto3 type that reaches no required field,
// and a second schema's proto2 type under that last one's name whose own field
// is required: a daemon whose plugin changed the type between two refreshes.
func requiredFiles(t *testing.T) (outer, plain, renamed protoreflect.MessageDescriptor) {
	t.Helper()
	required := descriptorpb.FieldDescriptorProto_LABEL_REQUIRED
	repeated := descriptorpb.FieldDescriptorProto_LABEL_REPEATED
	inner := &descriptorpb.FileDescriptorProto{
		Name: proto.String("inner.proto"), Package: proto.String("req"), Syntax: proto.String("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{{
			Name: proto.String("Inner"),
			Field: []*descriptorpb.FieldDescriptorProto{{
				Name: proto.String("x"), Number: proto.Int32(1), Label: &required,
				Type: descriptorpb.FieldDescriptorProto_TYPE_UINT32.Enum(),
			}},
		}},
	}
	message := descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum()
	outerFile := &descriptorpb.FileDescriptorProto{
		Name: proto.String("outer.proto"), Package: proto.String("req"), Syntax: proto.String("proto3"),
		Dependency: []string{"inner.proto"},
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: proto.String("Outer"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: proto.String("one"), Number: proto.Int32(1), Type: message, TypeName: proto.String(".req.Inner")},
					{Name: proto.String("many"), Number: proto.Int32(2), Type: message, TypeName: proto.String(".req.Inner"), Label: &repeated},
				},
			},
			{
				Name:  proto.String("Plain"),
				Field: []*descriptorpb.FieldDescriptorProto{scalar("n", 3, descriptorpb.FieldDescriptorProto_TYPE_UINT32)},
			},
		},
	}
	files, err := protodesc.NewFiles(&descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{inner, outerFile}})
	if err != nil {
		t.Fatal(err)
	}
	o, err := files.FindDescriptorByName("req.Outer")
	if err != nil {
		t.Fatal(err)
	}
	p, err := files.FindDescriptorByName("req.Plain")
	if err != nil {
		t.Fatal(err)
	}
	second, err := protodesc.NewFile(&descriptorpb.FileDescriptorProto{
		Name: proto.String("second.proto"), Package: proto.String("req"), Syntax: proto.String("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{{
			Name: proto.String("Plain"),
			Field: []*descriptorpb.FieldDescriptorProto{{
				Name: proto.String("n"), Number: proto.Int32(3), Label: &required,
				Type: descriptorpb.FieldDescriptorProto_TYPE_UINT32.Enum(),
			}},
		}},
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
	return o.(protoreflect.MessageDescriptor), p.(protoreflect.MessageDescriptor), second.Messages().ByName("Plain")
}

// Packing skips the required-field check only for a type that reaches no
// required field: one reached through a proto3 type, or arriving with a newer
// schema under a name already packed, is still refused unset, as anypb.New
// refuses it.
func TestUpsertRefusesAnUnsetRequiredFieldAnywhere(t *testing.T) {
	outer, plain, renamed := requiredFiles(t)
	r := Resource{Name: "req/rules", Kind: pb.TransactionResource_KIND_MAP, KeyType: "req.Outer", ValueType: "req.Outer"}
	rp := Resource{Name: "req/plain", Kind: pb.TransactionResource_KIND_MAP, KeyType: "req.Plain", ValueType: "req.Outer"}
	complete := dynamicpb.NewMessage(outer) // nothing set: no Inner to be incomplete
	one := dynamicpb.NewMessage(outer)
	oneFD := outer.Fields().ByName("one")
	one.Set(oneFD, protoreflect.ValueOfMessage(dynamicpb.NewMessage(oneFD.Message())))
	many := dynamicpb.NewMessage(outer)
	manyFD := outer.Fields().ByName("many")
	many.Mutable(manyFD).List().Append(protoreflect.ValueOfMessage(dynamicpb.NewMessage(manyFD.Message())))

	tx := (&Client{}).Transaction()
	if err := tx.Upsert(r, complete, complete); err != nil {
		t.Fatalf("a message with no Inner was refused: %v", err)
	}
	// Packed first, req.Plain is the name a stale packing would skip the check
	// for.
	if err := tx.Upsert(rp, dynamicpb.NewMessage(plain), complete); err != nil {
		t.Fatalf("a proto3 message was refused: %v", err)
	}
	for _, c := range []struct {
		name string
		r    Resource
		m    proto.Message
	}{
		{"an Inner without x", r, one},
		{"a repeated Inner without x", r, many},
		{"the second schema's Plain without n", rp, dynamicpb.NewMessage(renamed)},
	} {
		err := tx.Upsert(c.r, c.m, complete)
		var invalid *InvalidRequestError
		if !errors.As(err, &invalid) {
			t.Errorf("%s: Upsert answered %v, want an InvalidRequestError", c.name, err)
		}
	}
	if len(tx.ops) != 2 {
		t.Errorf("the transaction holds %d operations, want the 2 accepted", len(tx.ops))
	}
}
