// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"bytes"
	"context"
	"os"
	"sync/atomic"
	"testing"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

// Fault injection drops an actual native APPLIED acknowledgement. Discovery,
// mutation, replay lookup and the final table read all use the real daemon.
// BESS_BENCH_URL must name an owned idle stage fixture, as for the live bench.
type loseNativeStreamAck struct {
	transport
	dropped atomic.Bool
}

func (t *loseNativeStreamAck) apply(ctx context.Context, request *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	response, md, err := t.transport.apply(ctx, request)
	if err == nil && response.GetRecord().GetOutcome() == pb.TransactionRecord_OUTCOME_APPLIED && t.dropped.CompareAndSwap(false, true) {
		return nil, nil, status.Error(codes.Unavailable, "injected lost native acknowledgement")
	}
	return response, md, err
}

func TestLiveTransactionStreamResolvesLostAcknowledgement(t *testing.T) {
	verifyNativeStreamRecovery(t, func(client *Client) {
		client.t = &loseNativeStreamAck{transport: client.t}
	})
}

func verifyNativeStreamRecovery(t *testing.T, inject func(*Client)) {
	t.Helper()
	url := os.Getenv("BESS_BENCH_URL")
	if url == "" {
		t.Skip("BESS_BENCH_URL must name an owned idle stage fixture")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	client, err := DialTransactionStreams(url, 2)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	r, err := client.Resource(ctx, "stage0/rules")
	if err != nil {
		t.Fatal(err)
	}
	key := Encoded{0x0a, 0x06, 0x0a, 0x04, 0x0d, 0x00, 0x00, 0x01}
	value := Encoded{0x08, 0x02}
	inject(client)
	tx := client.Transaction()
	if err = tx.UpsertEncoded(r, key, value); err != nil {
		t.Fatal(err)
	}
	defer func() {
		cleanupCtx, done := context.WithTimeout(context.Background(), 5*time.Second)
		defer done()
		erase := client.Transaction()
		if eraseErr := erase.EraseEncoded(r, key); eraseErr != nil {
			t.Error(eraseErr)
			return
		}
		if _, commitErr := erase.Commit(cleanupCtx); commitErr != nil {
			t.Error(commitErr)
		}
	}()
	applied, err := tx.Commit(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if !applied.Replayed || applied.RequestID != tx.RequestID || applied.DaemonEpoch != r.DaemonEpoch {
		t.Fatalf("lost result was not resolved under its original epoch: %+v", applied)
	}
	known, record, epoch, err := client.GetTransaction(ctx, tx.RequestID)
	if err != nil || !known || epoch != r.DaemonEpoch || record.GetGeneration() != applied.Generation {
		t.Fatalf("native replay record differs: known=%v epoch=%d err=%v", known, epoch, err)
	}
	snapshot, err := client.Pipeline(ctx, WithResources(r.Name))
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, contents := range snapshot.Pipeline.GetResources() {
		for _, entry := range contents.GetEntries() {
			if bytes.Equal(entry.GetKey().GetValue(), key) {
				if found || !bytes.Equal(entry.GetValue().GetValue(), value) {
					t.Fatal("native table does not contain the one expected committed value")
				}
				found = true
			}
		}
	}
	if !found {
		t.Fatal("resolved transaction is absent from native table")
	}
}

func TestLiveTransactionStreamClosePreservesBorrowedConnection(t *testing.T) {
	url := os.Getenv("BESS_BENCH_URL")
	if url == "" {
		t.Skip("BESS_BENCH_URL must name an owned idle stage fixture")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	conn, err := grpc.NewClient(url, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	client, err := NewClientWithTransactionStreams(conn, DefaultRetryPolicy(), 1)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	r, err := client.Resource(ctx, "stage0/rules")
	if err != nil {
		t.Fatal(err)
	}
	key := Encoded{0x0a, 0x06, 0x0a, 0x04, 0x0d, 0x00, 0x00, 0x02}
	tx := client.Transaction()
	if err = tx.UpsertEncoded(r, key, Encoded{0x08, 0x01}); err != nil {
		t.Fatal(err)
	}
	if _, err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if err = client.Close(); err != nil {
		t.Fatal(err)
	}
	// This is a distinct client on the still-live borrowed channel. Its erase
	// proves both the connection and the native value survived stream teardown.
	other := NewClient(conn, DefaultRetryPolicy())
	r, err = other.Resource(ctx, r.Name)
	if err != nil {
		t.Fatal(err)
	}
	erase := other.Transaction()
	if err := erase.EraseEncoded(r, key); err != nil {
		t.Fatal(err)
	}
	if _, err := erase.Commit(ctx); err != nil {
		t.Fatal(err)
	}
}

type corruptNativeAckClient struct {
	pb.ControlClient
	missingRecord bool
	maxAckBytes   int
	corrupted     atomic.Bool
}

func (c *corruptNativeAckClient) TransactionChannel(ctx context.Context, opts ...grpc.CallOption) (grpc.BidiStreamingClient[pb.TransactionChannelRequest, pb.TransactionChannelResponse], error) {
	if c.maxAckBytes > 0 {
		opts = append(opts, grpc.MaxCallRecvMsgSize(c.maxAckBytes))
	}
	stream, err := c.ControlClient.TransactionChannel(ctx, opts...)
	if err != nil {
		return nil, err
	}
	return &corruptNativeAckStream{Control_TransactionChannelClient: stream, client: c}, nil
}

type corruptNativeAckStream struct {
	pb.Control_TransactionChannelClient
	client *corruptNativeAckClient
}

func (s *corruptNativeAckStream) Recv() (*pb.TransactionChannelResponse, error) {
	reply, err := s.Control_TransactionChannelClient.Recv()
	if err == nil && reply.GetTransaction().GetRecord().GetOutcome() == pb.TransactionRecord_OUTCOME_APPLIED &&
		s.client.corrupted.CompareAndSwap(false, true) {
		if s.client.missingRecord {
			reply.GetTransaction().Record = nil
		} else {
			reply.GetTransaction().Record.RequestId = "a-different-native-request"
		}
	}
	return reply, err
}

func TestLiveTransactionStreamResolvesUnusableNativeAcknowledgements(t *testing.T) {
	for _, name := range []string{"missing-record", "foreign-record", "receive-limit"} {
		t.Run(name, func(t *testing.T) {
			verifyNativeStreamRecovery(t, func(client *Client) {
				streams := client.t.(*transactionStreams)
				fault := &corruptNativeAckClient{ControlClient: streams.client,
					missingRecord: name == "missing-record"}
				if name == "receive-limit" {
					fault.maxAckBytes = 16
				}
				streams.client = fault
			})
		})
	}
}
