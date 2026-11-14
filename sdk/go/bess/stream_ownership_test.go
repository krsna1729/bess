// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"context"
	"net"
	"runtime"
	"sync/atomic"
	"testing"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

type gatedSendClient struct {
	pb.ControlClient
	frames  atomic.Int32
	blocked chan struct{}
	release chan struct{}
}

func (c *gatedSendClient) TransactionChannel(ctx context.Context, opts ...grpc.CallOption) (grpc.BidiStreamingClient[pb.TransactionChannelRequest, pb.TransactionChannelResponse], error) {
	s, err := c.ControlClient.TransactionChannel(ctx, opts...)
	if err != nil {
		return nil, err
	}
	return &gatedSendStream{Control_TransactionChannelClient: s, owner: c}, nil
}

type gatedSendStream struct {
	pb.Control_TransactionChannelClient
	owner *gatedSendClient
}

func (s *gatedSendStream) Send(r *pb.TransactionChannelRequest) error { return s.SendMsg(r) }
func (s *gatedSendStream) SendMsg(v any) error {
	if s.owner.frames.Add(1) == 2 {
		close(s.owner.blocked)
		select {
		case <-s.owner.release:
		case <-s.Context().Done():
			return s.Context().Err()
		}
	}
	return s.Control_TransactionChannelClient.SendMsg(v)
}

type ownershipPeer struct {
	pb.UnimplementedControlServer
	frames       chan *pb.TransactionChannelRequest
	releaseFirst chan struct{}
}

func (s *ownershipPeer) TransactionChannel(stream grpc.BidiStreamingServer[pb.TransactionChannelRequest, pb.TransactionChannelResponse]) error {
	for {
		frame, err := stream.Recv()
		if err != nil {
			return err
		}
		s.frames <- frame
		if frame.AttemptId == 1 {
			select {
			case <-s.releaseFirst:
			case <-stream.Context().Done():
				return stream.Context().Err()
			}
		}
		reply := &pb.TransactionChannelResponse{AttemptId: frame.AttemptId}
		for _, req := range frame.Transactions {
			reply.Results = append(reply.Results, &pb.TransactionChannelResult{Result: &pb.TransactionChannelResult_Transaction{
				Transaction: &pb.ApplyTransactionResponse{DaemonEpoch: 7, Record: &pb.TransactionRecord{
					RequestId: req.RequestId, Outcome: pb.TransactionRecord_OUTCOME_APPLIED}}}})
		}
		if err := stream.Send(reply); err != nil {
			return err
		}
	}
}

func TestTransactionStreamUnknownDoesNotBorrowCallerBytesAfterReturn(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	peer := &ownershipPeer{frames: make(chan *pb.TransactionChannelRequest, 2), releaseFirst: make(chan struct{})}
	server := grpc.NewServer()
	pb.RegisterControlServer(server, peer)
	go server.Serve(listener)
	defer server.Stop()
	conn, err := grpc.NewClient(listener.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	client, err := NewClientWithTransactionStreams(conn, RetryPolicy{AttemptTimeout: 10 * time.Second, Attempts: 4}, 1)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	client.observeEpoch(7)
	streams := client.t.(*transactionStreams)
	gate := &gatedSendClient{ControlClient: streams.client, blocked: make(chan struct{}), release: make(chan struct{})}
	streams.client = gate
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	r := Resource{Name: "test/value", KeyType: "test.Key", ValueType: "test.Value", DaemonEpoch: 7}
	start := func(callCtx context.Context, id string, value Encoded) <-chan error {
		tx := client.Transaction(WithRequestID(id))
		if err := tx.UpsertEncoded(r, nil, value); err != nil {
			t.Fatal(err)
		}
		done := make(chan error, 1)
		go func() { _, err := tx.Commit(callCtx); done <- err }()
		return done
	}
	await := func(done <-chan error) error {
		t.Helper()
		select {
		case err := <-done:
			return err
		case <-ctx.Done():
			t.Fatal(ctx.Err())
			return nil
		}
	}
	frame := func() *pb.TransactionChannelRequest {
		t.Helper()
		select {
		case r := <-peer.frames:
			return r
		case <-ctx.Done():
			t.Fatal(ctx.Err())
			return nil
		}
	}
	first := start(ctx, "first", nil)
	frame()
	memberCtx, stopMember := context.WithCancel(ctx)
	defer stopMember()
	value := Encoded{0x08, 1}
	member := start(memberCtx, "cancelled", value)
	other := start(ctx, "other", Encoded{0x08, 2})
	for len(streams.queue) != 2 {
		if ctx.Err() != nil {
			t.Fatal(ctx.Err())
		}
		runtime.Gosched()
	}
	close(peer.releaseFirst)
	if err := await(first); err != nil {
		t.Fatal(err)
	}
	select {
	case <-gate.blocked:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	stopMember()
	as[*TransportError](t, await(member))
	// Commit returned UNKNOWN while its sibling keeps the unsent frame alive.
	// Reusing the input is now permitted; it must not change the native write.
	value[1] = 99
	close(gate.release)
	got := frame()
	found := false
	for _, req := range got.Transactions {
		if req.RequestId == "cancelled" {
			found = true
			if value := req.Ops[0].Value.Value; len(value) != 2 || value[1] != 1 {
				t.Fatalf("native write borrowed reused input: %v", value)
			}
		}
	}
	if !found {
		t.Fatal("cancelled transaction was not in the already-claimed frame")
	}
	if err := await(other); err != nil {
		t.Fatalf("sibling transaction: %v", err)
	}
}
