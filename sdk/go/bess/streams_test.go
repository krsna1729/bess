// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"context"
	"net"
	"testing"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

// An unresponsive peer exercises real HTTP/2 cancellation, not an echoed ACK.
type heldTransactionPeer struct {
	pb.UnimplementedControlServer
	received chan struct{}
}

func (s *heldTransactionPeer) TransactionChannel(stream grpc.BidiStreamingServer[pb.TransactionChannelRequest, pb.TransactionChannelResponse]) error {
	if _, err := stream.Recv(); err != nil {
		return err
	}
	select {
	case s.received <- struct{}{}:
	case <-stream.Context().Done():
		return status.FromContextError(stream.Context().Err()).Err()
	}
	<-stream.Context().Done()
	return status.FromContextError(stream.Context().Err()).Err()
}
func TestTransactionStreamDeadlineReleasesLeaseWithoutCancellingOtherCaller(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	peer := &heldTransactionPeer{received: make(chan struct{}, 3)}
	server := grpc.NewServer()
	pb.RegisterControlServer(server, peer)
	go server.Serve(listener)
	defer server.Stop()
	conn, err := grpc.NewClient(listener.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	client, err := NewClientWithTransactionStreams(conn, DefaultRetryPolicy(), 1)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	start := func(callCtx context.Context) <-chan error {
		result := make(chan error, 1)
		go func() {
			_, _, err := client.t.apply(callCtx, &pb.ApplyTransactionRequest{RequestId: "held"})
			result <- err
		}()
		return result
	}
	awaitReceipt := func() {
		t.Helper()
		select {
		case <-peer.received:
		case <-ctx.Done():
			t.Fatal("leased attempt did not reach peer")
		}
	}
	awaitCode := func(result <-chan error, want codes.Code) {
		t.Helper()
		select {
		case err := <-result:
			if status.Code(err) != want {
				t.Fatalf("got %v, want %v", err, want)
			}
		case <-ctx.Done():
			t.Fatal("cancelled attempt did not release its stream")
		}
	}
	firstCtx, stopFirst := context.WithCancel(ctx)
	defer stopFirst()
	first := start(firstCtx)
	awaitReceipt()
	waitingCtx, stopWaiting := context.WithTimeout(ctx, 10*time.Millisecond)
	defer stopWaiting()
	awaitCode(start(waitingCtx), codes.DeadlineExceeded)
	select {
	case err := <-first:
		t.Fatalf("waiting caller cancelled the leased caller: %v", err)
	default:
	}
	stopFirst()
	awaitCode(first, codes.Canceled)
	// A subsequent real frame must arrive on a new lease. The active-read
	// timeout must retain DeadlineExceeded, not the lease's internal Canceled.
	readCtx, stopRead := context.WithTimeout(ctx, 100*time.Millisecond)
	defer stopRead()
	read := start(readCtx)
	awaitReceipt()
	awaitCode(read, codes.DeadlineExceeded)
}

// Answers each transaction by request ID; "refused" gets a typed refusal.
type batchingTransactionPeer struct {
	pb.UnimplementedControlServer
	frames  chan int
	release chan struct{}
}

func (s *batchingTransactionPeer) TransactionChannel(stream grpc.BidiStreamingServer[pb.TransactionChannelRequest, pb.TransactionChannelResponse]) error {
	for first := true; ; first = false {
		frame, err := stream.Recv()
		if err != nil {
			return err
		}
		s.frames <- len(frame.GetTransactions())
		if first {
			<-s.release
		}
		reply := &pb.TransactionChannelResponse{AttemptId: frame.GetAttemptId()}
		for _, tx := range frame.GetTransactions() {
			result := &pb.TransactionChannelResult{Result: &pb.TransactionChannelResult_Transaction{
				Transaction: &pb.ApplyTransactionResponse{Record: &pb.TransactionRecord{RequestId: tx.GetRequestId()}}}}
			if tx.GetRequestId() == "refused" {
				result = &pb.TransactionChannelResult{StatusCode: int32(codes.FailedPrecondition), StatusMessage: "stale",
					Result: &pb.TransactionChannelResult_Error{Error: &pb.ErrorDetail{Code: pb.ErrorDetail_EPOCH_MISMATCH}}}
			}
			reply.Results = append(reply.Results, result)
		}
		if err := stream.Send(reply); err != nil {
			return err
		}
	}
}

func TestTransactionStreamQueuedCommitsShareFrameWithOwnResults(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	peer := &batchingTransactionPeer{frames: make(chan int, 4), release: make(chan struct{})}
	server := grpc.NewServer()
	pb.RegisterControlServer(server, peer)
	go server.Serve(listener)
	defer server.Stop()
	conn, err := grpc.NewClient(listener.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	client, err := NewClientWithTransactionStreams(conn, DefaultRetryPolicy(), 1)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	type answer struct {
		response *pb.ApplyTransactionResponse
		trailer  metadata.MD
		err      error
	}
	start := func(id string) <-chan answer {
		result := make(chan answer, 1)
		go func() {
			response, trailer, err := client.t.apply(ctx, &pb.ApplyTransactionRequest{RequestId: id})
			result <- answer{response, trailer, err}
		}()
		return result
	}
	frameSize := func() int {
		t.Helper()
		select {
		case n := <-peer.frames:
			return n
		case <-ctx.Done():
			t.Fatal("frame did not reach peer")
			return 0
		}
	}
	ids := []string{"first", "second", "refused", "fourth"}
	answers := []<-chan answer{start(ids[0])}
	if n := frameSize(); n != 1 {
		t.Fatalf("first frame carried %d transactions, want 1", n)
	}
	for _, id := range ids[1:] {
		answers = append(answers, start(id))
	}
	queue := client.t.(*transactionStreams).queue
	for len(queue) < len(ids)-1 {
		if ctx.Err() != nil {
			t.Fatal("callers did not queue")
		}
		time.Sleep(time.Millisecond)
	}
	close(peer.release)
	if n := frameSize(); n != len(ids)-1 {
		t.Fatalf("queued commits used a frame of %d, want %d", n, len(ids)-1)
	}
	for i, result := range answers {
		got := <-result
		if ids[i] == "refused" {
			if status.Code(got.err) != codes.FailedPrecondition || len(got.trailer.Get(errorDetailKey)) != 1 {
				t.Fatalf("refused member: %v %v", got.err, got.trailer)
			}
			continue
		}
		if got.err != nil || got.response.GetRecord().GetRequestId() != ids[i] {
			t.Fatalf("%s got %v %v", ids[i], got.response, got.err)
		}
	}
}

func TestTransactionStreamCancellationInterruptsLeaseCreation(t *testing.T) {
	dialStarted := make(chan struct{}, 1)
	conn, err := grpc.NewClient("passthrough:///blocked-stream-creation",
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) {
			select {
			case dialStarted <- struct{}{}:
			default:
			}
			<-ctx.Done()
			return nil, ctx.Err()
		}))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	client, err := NewClientWithTransactionStreams(conn, DefaultRetryPolicy(), 1)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	result := make(chan error, 1)
	go func() {
		_, _, err := client.t.apply(ctx, &pb.ApplyTransactionRequest{RequestId: "cancel-creation"})
		result <- err
	}()
	watchdog, done := context.WithTimeout(context.Background(), 5*time.Second)
	defer done()
	select {
	case <-dialStarted:
	case <-watchdog.Done():
		t.Fatal("connection attempt did not start")
	}
	cancel()
	select {
	case err := <-result:
		if status.Code(err) != codes.Canceled {
			t.Fatalf("stream creation returned %v, want caller cancellation", err)
		}
	case <-watchdog.Done():
		t.Fatal("stream creation ignored caller cancellation")
	}
}
