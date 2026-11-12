// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"context"
	"fmt"
	"sync/atomic"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// DialTransactionStreams keeps up to concurrency persistent transaction
// streams, each with one unacknowledged frame. A stream's worker sends every
// commit already queued, up to 64, in its next frame; it never waits to fill
// one. Each transaction keeps its own result. Other control and administration
// RPCs use the ordinary channel. A lost frame leaves its transactions UNKNOWN,
// each going through the same GetTransaction resolution as a lost unary
// response.
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

// maxFrameTransactions is the daemon's per-frame bound (kMaxChannelTransactions).
const maxFrameTransactions = 64

func enableTransactionStreams(c *Client, concurrency int) {
	ctx, cancel := context.WithCancel(context.Background())
	t := &transactionStreams{transport: c.t, client: c.control, ctx: ctx,
		idle:  make(chan *transactionStream, concurrency),
		queue: make(chan *queuedTransaction, concurrency*maxFrameTransactions)}
	for range concurrency {
		t.idle <- nil // a stream opened on first use
	}
	c.t = t
	c.closeTransport = cancel
}

type transactionStreams struct {
	transport
	client pb.ControlClient
	ctx    context.Context
	idle   chan *transactionStream
	queue  chan *queuedTransaction
}

type queuedTransaction struct {
	ctx     context.Context
	request *pb.ApplyTransactionRequest
	claimed atomic.Bool // by a caller to send it, or by its own caller to withdraw it
	done    chan streamResult
}

type streamResult struct {
	response *pb.ApplyTransactionResponse
	trailer  metadata.MD
	err      error
}

// apply sends request on an idle stream itself, with whatever is already
// queued, so a lone caller has no handoff. With no idle stream it queues, and
// waits for its result or for a stream, whichever comes first.
func (t *transactionStreams) apply(ctx context.Context, request *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	if err := ctx.Err(); err != nil {
		return nil, nil, status.FromContextError(err).Err()
	}
	q := &queuedTransaction{ctx: ctx, request: request, done: make(chan streamResult, 1)}
	select {
	case s := <-t.idle:
		q.claimed.Store(true)
		return t.drive(s, q)
	default:
	}
	select {
	case t.queue <- q:
	case <-ctx.Done():
		return nil, nil, status.FromContextError(ctx.Err()).Err()
	case <-t.ctx.Done():
		return nil, nil, status.FromContextError(t.ctx.Err()).Err()
	}
	idle := t.idle
	for {
		select {
		case r := <-q.done:
			return r.response, r.trailer, r.err
		case s := <-idle:
			if q.claimed.CompareAndSwap(false, true) {
				return t.drive(s, q)
			}
			t.idle <- s
			idle = nil // another caller sent it; wait for its result
		case <-ctx.Done():
			// Withdrawn before anyone took it, or sent and now UNKNOWN: either
			// way the commit path resolves it as a lost unary answer.
			q.claimed.Store(true)
			return nil, nil, status.FromContextError(ctx.Err()).Err()
		case <-t.ctx.Done():
			q.claimed.Store(true)
			return nil, nil, status.FromContextError(t.ctx.Err()).Err()
		}
	}
}

// drive sends q, which its caller has claimed, and the queued commits that fit,
// on s, then returns s to the idle set.
func (t *transactionStreams) drive(s *transactionStream, q *queuedTransaction) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	batch := []*queuedTransaction{q}
drain:
	for len(batch) < maxFrameTransactions {
		select {
		case other := <-t.queue:
			batch = t.take(batch, other)
		default:
			break drain
		}
	}
	if len(batch) == 1 {
		t.idle <- t.send(s, batch)
		r := <-q.done
		return r.response, r.trailer, r.err
	}
	// Other callers' answers must not hold this caller past its own deadline.
	go func() { t.idle <- t.send(s, batch) }()
	select {
	case r := <-q.done:
		return r.response, r.trailer, r.err
	case <-q.ctx.Done():
		return nil, nil, status.FromContextError(q.ctx.Err()).Err()
	case <-t.ctx.Done():
		return nil, nil, status.FromContextError(t.ctx.Err()).Err()
	}
}

func (t *transactionStreams) take(batch []*queuedTransaction, q *queuedTransaction) []*queuedTransaction {
	if !q.claimed.CompareAndSwap(false, true) {
		return batch // withdrawn, or already sent by another caller
	}
	if err := q.ctx.Err(); err != nil {
		q.done <- streamResult{err: status.FromContextError(err).Err()} // never sent
		return batch
	}
	return append(batch, q)
}

type transactionStream struct {
	ctx    context.Context
	stream pb.Control_TransactionChannelClient
	cancel context.CancelFunc
	next   uint64
}

// send writes one frame and answers each transaction in it. It returns the
// stream to reuse, or nil after any failure that retires it.
func (t *transactionStreams) send(s *transactionStream, batch []*queuedTransaction) (next *transactionStream) {
	if s == nil {
		ctx, cancel := context.WithCancel(t.ctx)
		s = &transactionStream{ctx: ctx, cancel: cancel}
	}
	// Once every caller in the frame stops waiting, nobody needs its answers:
	// cancel the stream, so a silent daemon cannot pin this caller's driver.
	var abandoned func() bool
	if len(batch) == 1 {
		stop := context.AfterFunc(batch[0].ctx, s.cancel)
		abandoned = func() bool { return !stop() }
	} else {
		var waiting atomic.Int32
		waiting.Store(int32(len(batch)))
		stops := make([]func() bool, len(batch))
		for i, q := range batch {
			stops[i] = context.AfterFunc(q.ctx, func() {
				if waiting.Add(-1) == 0 {
					s.cancel()
				}
			})
		}
		abandoned = func() bool {
			fired := 0
			for _, stop := range stops {
				if !stop() {
					fired++
				}
			}
			return fired == len(stops)
		}
	}
	defer func() {
		if abandoned() && next != nil {
			s.cancel()
			next = nil
		}
	}()
	fail := func(err error) *transactionStream {
		for _, q := range batch {
			if ctxErr := q.ctx.Err(); ctxErr != nil {
				q.done <- streamResult{err: status.FromContextError(ctxErr).Err()}
			} else {
				q.done <- streamResult{err: err}
			}
		}
		s.cancel()
		return nil
	}
	if s.stream == nil {
		stream, err := t.client.TransactionChannel(s.ctx, grpc.ForceCodecV2(transactionChannelCodec))
		if err != nil {
			return fail(err)
		}
		s.stream = stream
	}
	s.next++
	frame := &pb.TransactionChannelRequest{AttemptId: s.next,
		Transactions: make([]*pb.ApplyTransactionRequest, len(batch))}
	// The frame's shared budget is its latest member deadline; a member without
	// one leaves only the stream deadline. Members whose own deadline passes
	// first return to their callers and resolve as UNKNOWN.
	var latest time.Time
	bounded := true
	for i, q := range batch {
		frame.Transactions[i] = q.request
		if deadline, ok := q.ctx.Deadline(); !ok {
			bounded = false
		} else if deadline.After(latest) {
			latest = deadline
		}
	}
	if bounded {
		remaining := time.Until(latest)
		if remaining <= 0 {
			return fail(status.Error(codes.DeadlineExceeded, "transaction attempt expired"))
		}
		frame.TimeoutNs = remaining.Nanoseconds()
	}
	if err := s.stream.Send(frame); err != nil {
		return fail(err)
	}
	reply, err := s.stream.Recv()
	if err != nil {
		return fail(err)
	}
	if reply.GetAttemptId() != frame.GetAttemptId() {
		return fail(status.Error(codes.Unknown, "transaction acknowledgement does not match attempt"))
	}
	results := reply.GetResults()
	if len(results) == 0 {
		// A refused frame: nothing in it was applied.
		if reply.GetStatusCode() == 0 || reply.GetError() == nil {
			return fail(status.Error(codes.Unknown, "invalid transaction acknowledgement frame"))
		}
		r := frameResult(&pb.TransactionChannelResult{StatusCode: reply.GetStatusCode(),
			StatusMessage: reply.GetStatusMessage(),
			Result:        &pb.TransactionChannelResult_Error{Error: reply.GetError()}}, "")
		for _, q := range batch {
			q.done <- r
		}
		return s
	}
	if len(results) != len(batch) || reply.GetStatusCode() != 0 {
		return fail(status.Error(codes.Unknown, "invalid transaction acknowledgement frame"))
	}
	for i, q := range batch {
		q.done <- frameResult(results[i], q.request.GetRequestId())
	}
	return s
}

// frameResult is one transaction's answer; requestID is empty for a refused frame.
func frameResult(result *pb.TransactionChannelResult, requestID string) streamResult {
	code := codes.Code(result.GetStatusCode())
	if code == codes.OK {
		if requestID == "" || result.GetTransaction().GetRecord().GetRequestId() != requestID {
			return streamResult{err: status.Error(codes.Unknown, "invalid transaction acknowledgement record")}
		}
		return streamResult{response: result.GetTransaction()}
	}
	if code > codes.Unauthenticated || result.GetError() == nil {
		return streamResult{err: status.Error(codes.Unknown, "invalid transaction error acknowledgement")}
	}
	wire, err := proto.Marshal(result.GetError())
	if err != nil {
		return streamResult{err: status.Error(codes.Unknown, "cannot decode transaction error acknowledgement")}
	}
	return streamResult{trailer: metadata.Pairs(errorDetailKey, string(wire)), err: status.Error(code, result.GetStatusMessage())}
}
