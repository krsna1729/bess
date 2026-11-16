// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"context"
	"net"
	"runtime"
	"testing"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/anypb"
)

// A real stalled frame owns its budget until its driver exits. Withdrawn
// requests must release theirs without a response or another queue drain.
func TestTransactionStreamByteAdmissionAndWithdrawal(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	peer := &heldTransactionPeer{received: make(chan struct{}, 2)}
	server := grpc.NewServer(grpc.MaxRecvMsgSize(MaxMessageBytes))
	pb.RegisterControlServer(server, peer)
	go server.Serve(listener)
	defer server.Stop()
	conn, err := grpc.NewClient(listener.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithDefaultCallOptions(grpc.MaxCallSendMsgSize(MaxMessageBytes)))
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
	ctx, stop := context.WithTimeout(context.Background(), 10*time.Second)
	defer stop()
	// The requests share backing bytes; admission must count their distinct
	// wire payloads, not depend on this test's physical allocation shortcut.
	payload := make([]byte, 32<<20)
	request := func(id string, bytes int) *pb.ApplyTransactionRequest {
		return &pb.ApplyTransactionRequest{RequestId: id, DaemonEpoch: 7, Ops: []*pb.TransactionOp{{
			Resource: "test/value", Value: &anypb.Any{Value: payload[:bytes]},
		}}}
	}
	start := func(callCtx context.Context, req *pb.ApplyTransactionRequest) <-chan error {
		done := make(chan error, 1)
		go func() { _, commitErr := client.commit(callCtx, req, 7); done <- commitErr }()
		return done
	}
	await := func(done <-chan error) error {
		t.Helper()
		select {
		case commitErr := <-done:
			return commitErr
		case <-ctx.Done():
			t.Fatal(ctx.Err())
			return nil
		}
	}
	awaitPending := func(minimum int64) {
		t.Helper()
		for streams.pendingBytes.Load() < minimum {
			if ctx.Err() != nil {
				t.Fatal(ctx.Err())
			}
			runtime.Gosched()
		}
	}
	firstCtx, stopFirst := context.WithCancel(ctx)
	defer stopFirst()
	first := start(firstCtx, request("active", 24<<20))
	select {
	case <-peer.received:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	waitingCtx, stopWaiting := context.WithCancel(ctx)
	defer stopWaiting()
	waiting := start(waitingCtx, request("withdrawn", 24<<20))
	awaitPending(48 << 20)
	_, err = client.commit(ctx, request("refused", 32<<20), 7)
	if e := as[*InvalidRequestError](t, err); e.Code != codes.ResourceExhausted {
		t.Fatalf("local capacity refusal: %v", e)
	}
	stopWaiting()
	if waitErr := await(waiting); status.Code(waitErr) != codes.Canceled {
		t.Fatalf("withdrawn request: %v", waitErr)
	}
	// The withdrawn entry is still in the channel, but its bytes must no
	// longer prevent a larger replacement from being admitted.
	replacementCtx, stopReplacement := context.WithCancel(ctx)
	defer stopReplacement()
	replacement := start(replacementCtx, request("replacement", 32<<20))
	awaitPending(56 << 20)
	stopReplacement()
	if replacementErr := await(replacement); status.Code(replacementErr) != codes.Canceled {
		t.Fatalf("replacement request: %v", replacementErr)
	}
	stopFirst()
	as[*TransportError](t, await(first)) // sent cancellation is still UNKNOWN
	if used := streams.pendingBytes.Load(); used != 0 {
		t.Fatalf("retained budget after drivers exit: %d", used)
	}
	oversized := request("oversized", 32<<20)
	oversized.Ops = append(oversized.Ops, oversized.Ops[0])
	for range cap(streams.pending) + 1 {
		_, err = client.commit(ctx, oversized, 7)
		as[*InvalidRequestError](t, err) // local refusals must return their count reservation
	}
	// A series of oversized local requests must not consume capacity forever.
	healthyCtx, stopHealthy := context.WithCancel(ctx)
	defer stopHealthy()
	healthy := start(healthyCtx, request("healthy-after-refusals", 0))
	select {
	case <-peer.received:
	case err := <-healthy:
		t.Fatalf("healthy request refused after local failures: %v", err)
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	stopHealthy()
	as[*TransportError](t, await(healthy))
}
