// SPDX-License-Identifier: BSD-3-Clause

// Benchmarks for this SDK's own path: the client side of a transaction over a
// real gRPC connection, with a stub daemon behind it. They need no bessd, so a
// change to this package can be measured in CI and in a plain
// `go test -bench`, and they are the client-side half of the control path's
// cost (the live benchmarks in tests/perf measure the rest).
//
// A transaction's per-operation cost is what the numbers should be read as:
// daemon-side operations, not sessions, is the unit that makes different
// clients and graphs comparable.
package bess

import (
	"context"
	"fmt"
	"net"
	"testing"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// stubControl is a daemon that answers what a transaction needs and does
// nothing else.
type stubControl struct {
	pb.UnimplementedControlServer
	resources *pb.ListTransactionResourcesResponse
	types     protoreflect.FileDescriptor
}

func (s *stubControl) GetCapabilities(context.Context, *pb.GetCapabilitiesRequest) (*pb.GetCapabilitiesResponse, error) {
	return &pb.GetCapabilitiesResponse{DaemonEpoch: 7, DaemonVersion: "bench"}, nil
}

func (s *stubControl) GetSchema(context.Context, *pb.GetSchemaRequest) (*pb.GetSchemaResponse, error) {
	return &pb.GetSchemaResponse{DaemonEpoch: 7}, nil
}

func (s *stubControl) ListTransactionResources(context.Context, *pb.ListTransactionResourcesRequest) (*pb.ListTransactionResourcesResponse, error) {
	return s.resources, nil
}

// ApplyTransaction answers APPLIED with a record the client takes at face value,
// so Commit takes its fast path (a status query would measure the other one).
// Ops carries one result per operation, as the real service returns them.
func (s *stubControl) ApplyTransaction(_ context.Context, in *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, error) {
	ops := make([]*pb.TransactionOpResult, 0, len(in.GetOps()))
	for range in.GetOps() {
		ops = append(ops, &pb.TransactionOpResult{Status: pb.TransactionOpResult_STATUS_APPLIED})
	}
	return &pb.ApplyTransactionResponse{Record: &pb.TransactionRecord{
		RequestId: in.GetRequestId(),
		Outcome:   pb.TransactionRecord_OUTCOME_APPLIED,
		Ops:       ops,
	}}, nil
}

// benchSchema is a descriptor set with the two messages a rule's key and value
// look like: byte-string masks on both sides and the identifiers a rule carries.
// It is deliberately close to the plugin's own tables, because the conversion's
// cost follows the field kinds.
func benchSchema() *descriptorpb.FileDescriptorProto {
	return &descriptorpb.FileDescriptorProto{
		Name:    proto.String("bench.proto"),
		Package: proto.String("bench"),
		Syntax:  proto.String("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: proto.String("RuleKey"),
				Field: []*descriptorpb.FieldDescriptorProto{
					scalar("address", 1, descriptorpb.FieldDescriptorProto_TYPE_BYTES),
					scalar("port", 2, descriptorpb.FieldDescriptorProto_TYPE_UINT32),
					scalar("proto", 3, descriptorpb.FieldDescriptorProto_TYPE_UINT32),
				},
			},
			{
				Name: proto.String("RuleValue"),
				Field: []*descriptorpb.FieldDescriptorProto{
					scalar("action_id", 1, descriptorpb.FieldDescriptorProto_TYPE_UINT32),
					scalar("mask", 2, descriptorpb.FieldDescriptorProto_TYPE_BYTES),
					scalar("gate", 3, descriptorpb.FieldDescriptorProto_TYPE_UINT32),
					scalar("meter_id", 4, descriptorpb.FieldDescriptorProto_TYPE_UINT32),
				},
			},
		},
	}
}

func scalar(name string, number int32, kind descriptorpb.FieldDescriptorProto_Type) *descriptorpb.FieldDescriptorProto {
	label := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
	return &descriptorpb.FieldDescriptorProto{
		Name: proto.String(name), Number: proto.Int32(number),
		Type: kind.Enum(), Label: &label,
	}
}

// benchClient is an SDK client on a bufconn connection to the stub, with the
// rule resource bound.
func benchClient(b *testing.B, rules int) (*Client, Resource, protoreflect.FileDescriptor) {
	b.Helper()
	file, err := protodesc.NewFile(benchSchema(), nil)
	if err != nil {
		b.Fatalf("descriptor: %v", err)
	}

	stub := &stubControl{
		types: file,
		resources: &pb.ListTransactionResourcesResponse{Resources: []*pb.TransactionResource{
			{
				Name:      "bench/rules",
				Kind:      pb.TransactionResource_KIND_MAP,
				KeyType:   "bench.RuleKey",
				ValueType: "bench.RuleValue",
			},
		}},
	}

	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	pb.RegisterControlServer(srv, stub)
	go func() { _ = srv.Serve(lis) }()
	b.Cleanup(srv.Stop)

	c, err := Dial("passthrough:///bench",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		b.Fatalf("dial: %v", err)
	}
	b.Cleanup(func() { _ = c.Close() })

	r, err := c.Resource(context.Background(), "bench/rules")
	if err != nil {
		b.Fatalf("resource: %v", err)
	}
	return c, r, file
}

// benchMessage is one message of the synthetic schema with every field set: the
// caller's side of a change, as dynamic as a schema-driven controller's.
func benchMessage(file protoreflect.FileDescriptor, name string, i int) proto.Message {
	m := dynamicpb.NewMessage(file.Messages().ByName(protoreflect.Name(name)))
	md := m.Descriptor()
	for j := 0; j < md.Fields().Len(); j++ {
		fd := md.Fields().Get(j)
		switch fd.Kind() {
		case protoreflect.BytesKind:
			m.Set(fd, protoreflect.ValueOfBytes([]byte{10, byte(i >> 8), byte(i), 0}))
		case protoreflect.Uint32Kind, protoreflect.Int32Kind:
			m.Set(fd, protoreflect.ValueOfUint32(uint32(i+1)))
		}
	}
	return m
}

// BenchmarkCommit is one transaction of `ops` upserts, built and committed: the
// SDK's bookkeeping, request assembly, protobuf marshalling, the gRPC round trip
// on a loopback connection, and the response's handling. The transaction is
// built inside the loop because a Commit may be made once -- a second call
// answers from its recorded result without an RPC -- and the build is included
// so that the difference from BenchmarkBuild is the round trip.
func BenchmarkCommit(b *testing.B) {
	for _, ops := range benchOps {
		b.Run(opName(ops), func(b *testing.B) {
			c, r, file := benchClient(b, ops)
			ctx := context.Background()
			keys := make([]proto.Message, ops)
			values := make([]proto.Message, ops)
			for i := 0; i < ops; i++ {
				keys[i] = benchMessage(file, "RuleKey", i)
				values[i] = benchMessage(file, "RuleValue", i)
			}
			b.ReportAllocs()
			for b.Loop() {
				tx := c.Transaction()
				for i := 0; i < ops; i++ {
					if err := tx.Upsert(r, keys[i], values[i]); err != nil {
						b.Fatal(err)
					}
				}
				if _, err := tx.Commit(ctx); err != nil {
					b.Fatal(err)
				}
			}
			b.ReportMetric(float64(ops), "daemon-ops/op")
		})
	}
}

// BenchmarkBuild is the same transaction without committing it: the SDK's own
// bookkeeping around keys and values, which is the part a caller repeats for
// every operation of every session.
func BenchmarkBuild(b *testing.B) {
	for _, ops := range benchOps {
		b.Run(opName(ops), func(b *testing.B) {
			c, r, file := benchClient(b, ops)
			keys := make([]proto.Message, ops)
			values := make([]proto.Message, ops)
			for i := 0; i < ops; i++ {
				keys[i] = benchMessage(file, "RuleKey", i)
				values[i] = benchMessage(file, "RuleValue", i)
			}
			b.ReportAllocs()
			for b.Loop() {
				tx := c.Transaction()
				for i := 0; i < ops; i++ {
					if err := tx.Upsert(r, keys[i], values[i]); err != nil {
						b.Fatal(err)
					}
				}
			}
			b.ReportMetric(float64(ops), "daemon-ops/op")
		})
	}
}

// benchOps is the operation count each benchmark sweeps: one operation (the
// floor a transaction's fixed cost shows at), two (the shape BESS's live
// transaction bench drives), eight (the session the performance contract
// records), eleven (today's session: six resources, eleven records) and 32 (the
// client's in-flight bound, an upper shape). Latency against this sweep is the
// comparison that separates a fixed cost from a per-operation one.
var benchOps = []int{1, 2, 8, 11, 32}

func opName(ops int) string { return fmt.Sprintf("%dops", ops) }
