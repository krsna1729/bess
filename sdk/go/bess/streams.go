// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"context"
	"fmt"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// DialTransactionStreams keeps a bounded set of persistent transaction streams.
// Each leased stream has exactly one unacknowledged attempt. Other control and
// administration RPCs use the ordinary channel. A lost stream is UNKNOWN and
// goes through the same GetTransaction resolution as a lost unary response.
func DialTransactionStreams(target string, concurrency int, opts ...grpc.DialOption) (*Client, error) {
	if concurrency < 1 || concurrency > 64 {
		return nil, fmt.Errorf("bess: transaction streams require 1..64 callers")
	}
	c, err := Dial(target, opts...)
	if err != nil {
		return nil, err
	}
	enableTransactionStreams(c, concurrency)
	return c, nil
}

// NewClientWithTransactionStreams borrows conn, like NewClient. Close cancels
// the client's streams but does not close the borrowed connection.
func NewClientWithTransactionStreams(conn grpc.ClientConnInterface, retry RetryPolicy, concurrency int) (*Client, error) {
	if concurrency < 1 || concurrency > 64 {
		return nil, fmt.Errorf("bess: transaction streams require 1..64 callers")
	}
	c := NewClient(conn, retry)
	enableTransactionStreams(c, concurrency)
	return c, nil
}

func enableTransactionStreams(c *Client, concurrency int) {
	ctx, cancel := context.WithCancel(context.Background())
	c.t = &transactionStreams{transport: c.t, client: c.control, ctx: ctx,
		slots: make(chan struct{}, concurrency), idle: make(chan *transactionStream, concurrency)}
	c.closeTransport = cancel
}

type transactionStreams struct {
	transport
	client pb.ControlClient
	ctx    context.Context
	slots  chan struct{}
	idle   chan *transactionStream
}

type transactionStream struct {
	stream pb.Control_TransactionChannelClient
	cancel context.CancelFunc
	next   uint64
}

func (t *transactionStreams) apply(ctx context.Context, request *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	select {
	case t.slots <- struct{}{}:
	case <-ctx.Done():
		return nil, nil, status.FromContextError(ctx.Err()).Err()
	case <-t.ctx.Done():
		return nil, nil, status.FromContextError(t.ctx.Err()).Err()
	}
	defer func() { <-t.slots }()
	if err := ctx.Err(); err != nil {
		return nil, nil, status.FromContextError(err).Err()
	}
	var lease *transactionStream
	var streamCtx context.Context
	select {
	case lease = <-t.idle:
	default:
		var cancel context.CancelFunc
		streamCtx, cancel = context.WithCancel(t.ctx)
		lease = &transactionStream{cancel: cancel}
	}
	// Cancelling an attempt retires its entire lease. It cannot cancel another
	// caller: no other attempt is ever outstanding on that stream.
	stopCancel := context.AfterFunc(ctx, lease.cancel)
	reusable := false
	defer func() {
		stopped := stopCancel()
		if reusable && stopped && ctx.Err() == nil && t.ctx.Err() == nil {
			t.idle <- lease
		} else {
			lease.cancel()
		}
	}()
	if lease.stream == nil {
		var err error
		lease.stream, err = t.client.TransactionChannel(streamCtx)
		if err != nil {
			if ctxErr := ctx.Err(); ctxErr != nil {
				err = status.FromContextError(ctxErr).Err()
			}
			return nil, nil, err
		}
	}
	lease.next++
	frame := &pb.TransactionChannelRequest{AttemptId: lease.next, Transaction: request}
	if deadline, ok := ctx.Deadline(); ok {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return nil, nil, status.Error(codes.DeadlineExceeded, "transaction attempt expired")
		}
		frame.TimeoutNs = remaining.Nanoseconds()
	}
	if err := lease.stream.Send(frame); err != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			err = status.FromContextError(ctxErr).Err()
		}
		return nil, nil, err
	}
	reply, err := lease.stream.Recv()
	if err != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			err = status.FromContextError(ctxErr).Err()
		}
		return nil, nil, err
	}
	if reply.GetAttemptId() != frame.GetAttemptId() {
		return nil, nil, status.Error(codes.Unknown, "transaction acknowledgement does not match attempt")
	}
	code := codes.Code(reply.GetStatusCode())
	if code == codes.OK {
		if reply.GetTransaction() == nil || reply.GetError() != nil ||
			reply.GetTransaction().GetRecord() == nil ||
			reply.GetTransaction().GetRecord().GetRequestId() != request.GetRequestId() {
			return nil, nil, status.Error(codes.Unknown, "invalid transaction acknowledgement record")
		}
		reusable = true
		return reply.GetTransaction(), nil, nil
	}
	if code > codes.Unauthenticated || reply.GetError() == nil || reply.GetTransaction() != nil {
		return nil, nil, status.Error(codes.Unknown, "invalid transaction error acknowledgement")
	}
	wire, err := proto.Marshal(reply.GetError())
	if err != nil {
		return nil, nil, status.Error(codes.Unknown, "cannot decode transaction error acknowledgement")
	}
	reusable = true
	return nil, metadata.Pairs(errorDetailKey, string(wire)), status.Error(code, reply.GetStatusMessage())
}
