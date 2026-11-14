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
	"google.golang.org/protobuf/encoding/protowire"
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

// All accepted requests together fit one maximum-sized frame, including the
// largest attempt ID and timeout. This bounds retained wire payloads even when
// callers use every stream and the count-bounded queue at once.
const maxPendingTransactionBytes = MaxMessageBytes - 22

func enableTransactionStreams(c *Client, concurrency int) {
	ctx, cancel := context.WithCancel(context.Background())
	t := &transactionStreams{transport: c.t, client: c.control, ctx: ctx,
		idle:    make(chan *transactionStream, concurrency),
		queue:   make(chan *transactionSlot, concurrency*maxFrameTransactions),
		pending: make(chan struct{}, 2*concurrency*maxFrameTransactions)}
	for range concurrency {
		t.idle <- nil // a stream opened on first use
	}
	c.t = t
	c.closeTransport = cancel
}

type transactionStreams struct {
	transport
	client       pb.ControlClient
	ctx          context.Context
	idle         chan *transactionStream
	queue        chan *transactionSlot
	pending      chan struct{} // queued plus active requests, reserved before encoding
	pendingBytes atomic.Int64
}

type queuedTransaction struct {
	ctx     context.Context
	id      string
	wire    []byte      // owned before a caller can return while a batch driver is sending
	claimed atomic.Bool // by a caller to send it, or by its own caller to withdraw it
	done    chan streamResult
	bytes   int64 // request plus its repeated-message framing
	slot    *transactionSlot
}

// A stale channel entry must not retain a completed result, a large request ID,
// or a caller's context values. Its separately allocated slot is detached when
// claimed or withdrawn; the first, unqueued caller needs no slot allocation.
type transactionSlot struct {
	request atomic.Pointer[queuedTransaction]
}

type streamResult struct {
	response *pb.ApplyTransactionResponse
	trailer  metadata.MD
	err      error
}

// notSent distinguishes local admission from an unanswered RPC. In particular,
// a full local queue must not trigger GetTransaction for a request never sent.
type notSent struct{ error }

func (e *notSent) Unwrap() error { return e.error }

func localCapacityRefusal() error {
	const message = "client transaction budget exhausted"
	return &notSent{&InvalidRequestError{Code: codes.ResourceExhausted, Message: message}}
}

func (t *transactionStreams) release(q *queuedTransaction) {
	// Only the claimant releases a request. A withdrawn queue entry may stay
	// in the channel until the next drain, but no longer pins its payload.
	if q.slot != nil {
		q.slot.request.Store(nil)
	}
	q.id = ""
	q.wire = nil
	t.pendingBytes.Add(-q.bytes)
	<-t.pending
}

// apply sends request on an idle stream itself, with whatever is already
// queued, so a lone caller has no handoff. With no idle stream it queues, and
// waits for its result or for a stream, whichever comes first.
func (t *transactionStreams) apply(ctx context.Context, request *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	if err := ctx.Err(); err != nil {
		return nil, nil, &notSent{status.FromContextError(err).Err()}
	}
	if err := t.ctx.Err(); err != nil {
		return nil, nil, &notSent{status.FromContextError(err).Err()}
	}
	select {
	case t.pending <- struct{}{}:
	default:
		return nil, nil, localCapacityRefusal()
	}
	size := int64(protowire.SizeTag(2) + protowire.SizeBytes(proto.Size(request)))
	if size > maxPendingTransactionBytes {
		<-t.pending
		return nil, nil, &notSent{&InvalidRequestError{Code: codes.ResourceExhausted, Message: "transaction exceeds the stream message budget"}}
	}
	for {
		used := t.pendingBytes.Load()
		if size > maxPendingTransactionBytes-used {
			<-t.pending
			return nil, nil, localCapacityRefusal()
		}
		if t.pendingBytes.CompareAndSwap(used, used+size) {
			break
		}
	}
	// Snapshot on this caller, before handing off to a shared frame. Another
	// member's deadline may keep that frame alive after this caller returns.
	wire, err := (proto.MarshalOptions{UseCachedSize: true}).Marshal(request)
	if err != nil {
		t.pendingBytes.Add(-size)
		<-t.pending
		return nil, nil, &notSent{&InvalidRequestError{Message: err.Error()}}
	}
	q := &queuedTransaction{ctx: ctx, id: request.GetRequestId(), wire: wire,
		done: make(chan streamResult, 1), bytes: size}
	select {
	case s := <-t.idle:
		q.claimed.Store(true)
		return t.drive(s, q)
	default:
	}
	q.slot = &transactionSlot{}
	q.slot.request.Store(q)
	select {
	case t.queue <- q.slot:
	case <-ctx.Done():
		t.release(q)
		return nil, nil, &notSent{status.FromContextError(ctx.Err()).Err()}
	case <-t.ctx.Done():
		t.release(q)
		return nil, nil, &notSent{status.FromContextError(t.ctx.Err()).Err()}
	default:
		t.release(q)
		return nil, nil, localCapacityRefusal()
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
			if q.claimed.CompareAndSwap(false, true) {
				t.release(q) // withdrawn before any driver could send it
				return nil, nil, &notSent{status.FromContextError(ctx.Err()).Err()}
			}
			return nil, nil, status.FromContextError(ctx.Err()).Err()
		case <-t.ctx.Done():
			if q.claimed.CompareAndSwap(false, true) {
				t.release(q)
				return nil, nil, &notSent{status.FromContextError(t.ctx.Err()).Err()}
			}
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

func (t *transactionStreams) take(batch []*queuedTransaction, slot *transactionSlot) []*queuedTransaction {
	q := slot.request.Swap(nil)
	if q == nil || !q.claimed.CompareAndSwap(false, true) {
		return batch // withdrawn, or already sent by another caller
	}
	if err := q.ctx.Err(); err != nil {
		t.release(q)
		q.done <- streamResult{err: &notSent{status.FromContextError(err).Err()}} // never sent
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
	defer func() {
		for _, q := range batch {
			t.release(q)
		}
	}()
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
	frame := &encodedTransactionFrame{attempt: s.next, requests: make([][]byte, len(batch))}
	// The frame's shared budget is its latest member deadline; a member without
	// one leaves only the stream deadline. Members whose own deadline passes
	// first return to their callers and resolve as UNKNOWN.
	var latest time.Time
	bounded := true
	for i, q := range batch {
		frame.requests[i] = q.wire
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
		frame.timeout = remaining.Nanoseconds()
	}
	if err := s.stream.SendMsg(frame); err != nil {
		return fail(err)
	}
	reply, err := s.stream.Recv()
	if err != nil {
		return fail(err)
	}
	if reply.GetAttemptId() != frame.attempt {
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
		q.done <- frameResult(results[i], q.id)
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
