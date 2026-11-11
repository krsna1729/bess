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
