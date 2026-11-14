// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
	"google.golang.org/protobuf/types/known/anypb"
)

func TestUncachedSchemaStillValidatesRequiredFieldsAndRoundTrips(t *testing.T) {
	// This legal name cannot fit either metadata cache. Packing must remain
	// correct, including proto2 presence checks, rather than skip validation.
	file, err := protodesc.NewFile(&descriptorpb.FileDescriptorProto{
		Name: proto.String("uncached.proto"), Syntax: proto.String("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{{
			Name: proto.String(strings.Repeat("X", maxTypeCacheBytes+1)),
			Field: []*descriptorpb.FieldDescriptorProto{{
				Name: proto.String("value"), Number: proto.Int32(1),
				Label: descriptorpb.FieldDescriptorProto_LABEL_REQUIRED.Enum(),
				Type:  descriptorpb.FieldDescriptorProto_TYPE_INT64.Enum(),
			}},
		}},
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
	md := file.Messages().Get(0)
	value := dynamicpb.NewMessage(md)
	var encoded anypb.Any
	if err := pack(&encoded, value); err == nil {
		t.Fatal("uncached packing accepted an unset required field")
	}
	field := md.Fields().Get(0)
	value.Set(field, protoreflect.ValueOfInt64(7))
	if err := pack(&encoded, value); err != nil {
		t.Fatal(err)
	}
	decoded := dynamicpb.NewMessage(md)
	if err := encoded.UnmarshalTo(decoded); err != nil {
		t.Fatal(err)
	}
	if got := decoded.Get(field).Int(); got != 7 {
		t.Fatalf("uncached round trip changed the value to %d", got)
	}
}
