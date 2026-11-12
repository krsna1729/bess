// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"testing"

	"google.golang.org/grpc/mem"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

func TestChannelCodecDecodesAcknowledgementsLikeProtobuf(t *testing.T) {
	applied := func(id string, replayed bool, ops ...*pb.TransactionOpResult) *pb.TransactionChannelResult {
		return &pb.TransactionChannelResult{Result: &pb.TransactionChannelResult_Transaction{
			Transaction: &pb.ApplyTransactionResponse{DaemonEpoch: 7, Replayed: replayed, Record: &pb.TransactionRecord{
				RequestId: id, Outcome: pb.TransactionRecord_OUTCOME_REJECTED, Generation: 42, Ops: ops,
				Visibility: pb.TransactionRecord_VISIBILITY_SCOPE_SNAPSHOT}}}}
	}
	marshal := func(m proto.Message) []byte {
		b, err := proto.Marshal(m)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	nest := func(num protowire.Number, inner []byte) []byte {
		return protowire.AppendBytes(protowire.AppendTag(nil, num, protowire.BytesType), inner)
	}
	twoApplied := &pb.TransactionChannelResponse{AttemptId: 9, Results: []*pb.TransactionChannelResult{
		applied("tx-1", false,
			&pb.TransactionOpResult{Status: pb.TransactionOpResult_STATUS_FAILED, Error: "no such key"},
			&pb.TransactionOpResult{Status: pb.TransactionOpResult_STATUS_NOT_APPLIED}),
		applied("tx-2", true),
	}}
	withUnknown := proto.Clone(twoApplied).(*pb.TransactionChannelResponse)
	withUnknown.Results[1].GetTransaction().Record.ProtoReflect().SetUnknown(
		protowire.AppendVarint(protowire.AppendTag(nil, 99, protowire.VarintType), 1))
	refusedTransaction := &pb.TransactionChannelResponse{AttemptId: 9, Results: []*pb.TransactionChannelResult{
		applied("tx-1", false),
		{StatusCode: 9, StatusMessage: "busy", Result: &pb.TransactionChannelResult_Error{
			Error: &pb.ErrorDetail{Code: pb.ErrorDetail_RESOURCE_BUSY, Message: "busy"}}},
	}}
	refusedFrame := &pb.TransactionChannelResponse{AttemptId: 9, StatusCode: 3, StatusMessage: "bad frame",
		Error: &pb.ErrorDetail{Code: pb.ErrorDetail_INVALID_ARGUMENT, Field: "transactions"}}

	cases := []struct {
		name  string
		frame []byte
		fast  bool
	}{
		{"applied transactions", marshal(twoApplied), true},
		{"unknown field", marshal(withUnknown), false},
		{"refused transaction", marshal(refusedTransaction), false},
		{"refused frame", marshal(refusedFrame), false},
		{"invalid UTF-8 request id", nest(2, nest(1, nest(1, nest(1, []byte{0xff})))), false},
		{"truncated", marshal(twoApplied)[:20], false},
		{"empty", nil, false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			want := new(pb.TransactionChannelResponse)
			wantErr := proto.Unmarshal(c.frame, want)
			got := new(pb.TransactionChannelResponse)
			gotErr := transactionChannelCodec.Unmarshal(mem.BufferSlice{mem.SliceBuffer(c.frame)}, got)
			if (gotErr != nil) != (wantErr != nil) {
				t.Fatalf("error %v, protobuf error %v", gotErr, wantErr)
			}
			if wantErr == nil && !proto.Equal(got, want) {
				t.Fatalf("decoded %v, protobuf decoded %v", got, want)
			}
			if fast := decodeChannelResponse(append([]byte(nil), c.frame...), new(pb.TransactionChannelResponse)); fast != c.fast {
				t.Fatalf("decoded without protobuf: %v, want %v", fast, c.fast)
			}
		})
	}
}
