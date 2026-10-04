// SPDX-License-Identifier: BSD-3-Clause

// Package bess is a thin client for BESS's generic control API (control_v2,
// roadmap M27). It has the same guarantees as Python's pybess.sdk
// (docs/control-sdk.md): one request id per logical transaction, kept across
// retries; after a send without an answer the outcome is asked for, never
// assumed, and the request is sent again only once GetTransaction answered
// "not known" under the daemon epoch the commit started with; an answer from
// any other epoch is ErrDaemonRestarted; typed resources are checked before
// anything is sent; a resource handle is bound to the daemon epoch it was
// discovered under, and one from before a restart the client has seen is
// refused (StaleResourceError), never sent; and outcomes are typed. It holds no application
// semantics: resources are whatever modules registered ("<module>/<table>"),
// keys and values their own protobuf messages.
//
//	client, err := bess.Dial("localhost:10514")
//	rules, err := client.Resource(ctx, "em0/rules")
//	tx := client.Transaction()
//	err = tx.Upsert(rules, key, value)
//	applied, err := tx.Commit(ctx)
package bess

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"maps"
	"strings"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/anypb"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// errorDetailKey is the trailer that carries a failed call's ErrorDetail.
const errorDetailKey = "bess-error-bin"

// MaxMessageBytes is what bessd accepts and sends (D-026).
const MaxMessageBytes = 64 << 20

// -- errors --------------------------------------------------------------------

// InvalidRequestError: refused before anything was applied (bad resource,
// key or value, or a request id reused with other contents).
type InvalidRequestError struct {
	Message string
	Code    codes.Code      // the call's status code (codes.OK: refused by the client)
	Detail  *pb.ErrorDetail // the server's detail, when it sent one
}

func (e *InvalidRequestError) Error() string { return "bess: invalid request: " + e.Message }

// ConflictError: expected_generation no longer matched; this attempt tried
// nothing. AfterUnknownAttempt: an earlier send of the same commit went
// unanswered and may have applied (its record since aged out): re-read the
// state.
type ConflictError struct {
	Record              *pb.TransactionRecord
	AfterUnknownAttempt bool
}

func (e *ConflictError) Error() string {
	s := fmt.Sprintf("bess: generation moved on (now %d)", e.Record.GetGeneration())
	if e.AfterUnknownAttempt {
		s += "; an earlier unanswered attempt may have applied"
	}
	return s
}

// OpFailure is one failing operation of a rejected transaction.
type OpFailure struct {
	Index int
	Error string
}

// RejectedError: an operation failed and nothing changed.
type RejectedError struct {
	Record   *pb.TransactionRecord
	Failures []OpFailure
}

func (e *RejectedError) Error() string {
	parts := make([]string, len(e.Failures))
	for i, f := range e.Failures {
		parts[i] = fmt.Sprintf("#%d %s", f.Index, f.Error)
	}
	return "bess: transaction rejected: " + strings.Join(parts, "; ")
}

// BusyError: the dataplane stayed busy through every attempt, and every send
// was answered (nothing applied).
type BusyError struct{ Attempts int }

func (e *BusyError) Error() string {
	return fmt.Sprintf("bess: still busy after %d attempts", e.Attempts)
}

// TransportError: no answer. The outcome is unknown, not failed; ask
// GetTransaction(RequestID) later.
type TransportError struct {
	RequestID string
	Code      codes.Code
}

func (e *TransportError) Error() string {
	return fmt.Sprintf("bess: no answer for %q: %s", e.RequestID, e.Code)
}

// DaemonRestartedError: the transaction met a daemon that lost the state it
// was built against. The restarted daemon may have applied the request (a
// restart the client had not seen yet): re-read its state before deciding
// again; a retry under a new request id could apply it twice.
type DaemonRestartedError struct{ From, To uint64 }

func (e *DaemonRestartedError) Error() string {
	return fmt.Sprintf("bess: the daemon restarted (epoch %d -> %d)", e.From, e.To)
}

// StaleResourceError: a resource handle, or a transaction built with one,
// from an earlier daemon epoch than the one the client has seen. Nothing was
// sent: look the resource up again and rebuild the transaction.
type StaleResourceError struct {
	Resource      string // empty: the transaction as a whole
	Bound, Daemon uint64
}

func (e *StaleResourceError) Error() string {
	what := "the transaction"
	if e.Resource != "" {
		what = fmt.Sprintf("resource %q", e.Resource)
	}
	return fmt.Sprintf("bess: %s belongs to daemon epoch %d, not %d: look the resource up again",
		what, e.Bound, e.Daemon)
}

// -- results -------------------------------------------------------------------

// Applied is a transaction that was applied.
type Applied struct {
	RequestID   string
	Generation  uint64
	Visibility  pb.TransactionRecord_Visibility
	Replayed    bool // an earlier identical request's recorded outcome
	DaemonEpoch uint64
	Record      *pb.TransactionRecord
}

// Resource is a transactional resource, its key and value message types, and
// the daemon epoch it was discovered under (valid only in that epoch).
type Resource struct {
	Name, KeyType, ValueType string
	DaemonEpoch              uint64
}

// RetryPolicy: how long one RPC may take and how many RPCs a commit may make
// (status queries included: 2 is one send plus one question).
type RetryPolicy struct {
	AttemptTimeout time.Duration
	Attempts       int
	BusyBackoff    time.Duration // doubled per BUSY answer
}

// DefaultRetryPolicy is 5 s per RPC, 4 RPCs, 10 ms first backoff.
func DefaultRetryPolicy() RetryPolicy {
	return RetryPolicy{AttemptTimeout: 5 * time.Second, Attempts: 4, BusyBackoff: 10 * time.Millisecond}
}

// -- transport -----------------------------------------------------------------

// transport is the three RPCs, with each failed call's trailers (tests
// replace it).
type transport interface {
	apply(ctx context.Context, in *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error)
	get(ctx context.Context, in *pb.GetTransactionRequest) (*pb.GetTransactionResponse, metadata.MD, error)
	list(ctx context.Context) (*pb.ListTransactionResourcesResponse, metadata.MD, error)
}

type grpcTransport struct{ c pb.ControlClient }

func (t grpcTransport) apply(ctx context.Context, in *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	var md metadata.MD
	r, err := t.c.ApplyTransaction(ctx, in, grpc.Trailer(&md))
	return r, md, err
}

func (t grpcTransport) get(ctx context.Context, in *pb.GetTransactionRequest) (*pb.GetTransactionResponse, metadata.MD, error) {
	var md metadata.MD
	r, err := t.c.GetTransaction(ctx, in, grpc.Trailer(&md))
	return r, md, err
}

func (t grpcTransport) list(ctx context.Context) (*pb.ListTransactionResourcesResponse, metadata.MD, error) {
	var md metadata.MD
	r, err := t.c.ListTransactionResources(ctx, &pb.ListTransactionResourcesRequest{}, grpc.Trailer(&md))
	return r, md, err
}

// -- the client ----------------------------------------------------------------

// Client is one daemon's control endpoint. Safe for concurrent use; a
// Transaction is not.
type Client struct {
	t     transport
	retry RetryPolicy
	sleep func(time.Duration)

	mu         sync.Mutex
	resources  map[string]Resource
	epoch      uint64
	epochKnown bool
	conn       *grpc.ClientConn
}

// Dial connects to bessd's gRPC endpoint (no TLS, like bessd).
func Dial(target string, opts ...grpc.DialOption) (*Client, error) {
	opts = append([]grpc.DialOption{
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(MaxMessageBytes),
			grpc.MaxCallSendMsgSize(MaxMessageBytes)),
	}, opts...)
	conn, err := grpc.NewClient(target, opts...)
	if err != nil {
		return nil, err
	}
	c := NewClient(conn, DefaultRetryPolicy())
	c.conn = conn
	return c, nil
}

// NewClient uses an existing connection.
func NewClient(conn grpc.ClientConnInterface, retry RetryPolicy) *Client {
	return newClient(grpcTransport{pb.NewControlClient(conn)}, retry, time.Sleep)
}

func newClient(t transport, retry RetryPolicy, sleep func(time.Duration)) *Client {
	if retry.Attempts < 1 {
		retry.Attempts = 1
	}
	return &Client{t: t, retry: retry, sleep: sleep}
}

// Close closes a connection Dial opened.
func (c *Client) Close() error {
	if c.conn != nil {
		return c.conn.Close()
	}
	return nil
}

// DaemonEpoch is the last epoch the daemon reported (false: none yet).
func (c *Client) DaemonEpoch() (uint64, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.epoch, c.epochKnown
}

// Resources lists every transactional resource, by name (cached unless
// refresh).
func (c *Client) Resources(ctx context.Context, refresh bool) (map[string]Resource, error) {
	c.mu.Lock()
	cached := maps.Clone(c.resources)
	c.mu.Unlock()
	if cached != nil && !refresh {
		return cached, nil // a copy: the caller may edit it
	}
	actx, cancel := context.WithTimeout(ctx, c.retry.AttemptTimeout)
	defer cancel()
	r, md, err := c.t.list(actx)
	if err != nil {
		if noAnswer(err, md) {
			return nil, &TransportError{Code: status.Code(err)}
		}
		return nil, translate(err, md)
	}
	found := make(map[string]Resource, len(r.GetResources()))
	for _, res := range r.GetResources() {
		found[res.GetName()] = Resource{res.GetName(), res.GetKeyType(), res.GetValueType(),
			r.GetDaemonEpoch()}
	}
	c.observeEpoch(r.GetDaemonEpoch())
	c.mu.Lock()
	c.resources = found
	c.mu.Unlock()
	return maps.Clone(found), nil
}

// Resource is the resource name; InvalidRequestError if no module
// registered it.
func (c *Client) Resource(ctx context.Context, name string) (Resource, error) {
	for _, refresh := range []bool{false, true} {
		all, err := c.Resources(ctx, refresh)
		if err != nil {
			return Resource{}, err
		}
		if r, ok := all[name]; ok {
			return r, nil
		}
	}
	return Resource{}, &InvalidRequestError{Message: fmt.Sprintf("no transactional resource %q", name)}
}

// GetTransaction asks for requestID's outcome under the current epoch.
func (c *Client) GetTransaction(ctx context.Context, requestID string) (known bool, rec *pb.TransactionRecord, epoch uint64, err error) {
	actx, cancel := context.WithTimeout(ctx, c.retry.AttemptTimeout)
	defer cancel()
	r, md, err := c.t.get(actx, &pb.GetTransactionRequest{RequestId: requestID})
	if err != nil {
		if noAnswer(err, md) {
			return false, nil, 0, &TransportError{RequestID: requestID, Code: status.Code(err)}
		}
		return false, nil, 0, translate(err, md)
	}
	return r.GetKnown(), r.GetRecord(), r.GetDaemonEpoch(), nil
}

// observeEpoch records the daemon's epoch; true if it changed. A changed
// epoch drops the resource cache (the restarted daemon's modules may differ).
func (c *Client) observeEpoch(epoch uint64) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	changed := c.epochKnown && epoch != c.epoch
	c.epoch, c.epochKnown = epoch, true
	if changed {
		c.resources = nil
	}
	return changed
}

func (c *Client) checkEpoch(answered, expected uint64) error {
	c.observeEpoch(answered)
	if answered != expected {
		return &DaemonRestartedError{From: expected, To: answered}
	}
	return nil
}

// -- transactions --------------------------------------------------------------

// TxOption configures a transaction.
type TxOption func(*Transaction)

// WithExpectedGeneration refuses the transaction (ConflictError) unless the
// dataplane generation is still g.
func WithExpectedGeneration(g uint64) TxOption {
	return func(t *Transaction) { t.expected, t.hasExpected = g, true }
}

// WithSnapshot asks for CONSISTENCY_SCOPE_SNAPSHOT (D-050); the default is
// referential.
func WithSnapshot() TxOption { return func(t *Transaction) { t.snapshot = true } }

// WithRequestID resumes a known request id (a client that restarted).
func WithRequestID(id string) TxOption { return func(t *Transaction) { t.RequestID = id } }

// Transaction is operations on any resources, applied all or nothing by
// Commit. Not safe for concurrent use.
type Transaction struct {
	// One id for the whole life of the transaction: a retry after a timeout
	// must be recognised as the same request.
	RequestID string

	c           *Client
	ops         []*pb.TransactionOp
	expected    uint64
	hasExpected bool
	snapshot    bool
	result      *Applied
	epoch       uint64 // the epoch of the resources the ops were built with
	bound       bool
}

// Transaction starts one.
func (c *Client) Transaction(opts ...TxOption) *Transaction {
	t := &Transaction{c: c}
	for _, o := range opts {
		o(t)
	}
	if t.RequestID == "" {
		var b [16]byte
		_, _ = rand.Read(b[:])
		t.RequestID = hex.EncodeToString(b[:])
	}
	return t
}

// bind checks that r belongs to the epoch the client has seen (when it has
// seen one; Commit checks again) and to this transaction's epoch.
func (t *Transaction) bind(r Resource) error {
	if epoch, known := t.c.DaemonEpoch(); known && r.DaemonEpoch != epoch {
		return &StaleResourceError{Resource: r.Name, Bound: r.DaemonEpoch, Daemon: epoch}
	}
	if t.bound && r.DaemonEpoch != t.epoch {
		return &StaleResourceError{Resource: r.Name, Bound: r.DaemonEpoch, Daemon: t.epoch}
	}
	t.epoch, t.bound = r.DaemonEpoch, true
	return nil
}

// Upsert adds key -> value to r, or replaces its value.
func (t *Transaction) Upsert(r Resource, key, value proto.Message) error {
	if err := t.bind(r); err != nil {
		return err
	}
	if err := checkType(r, "key", key, r.KeyType); err != nil {
		return err
	}
	if err := checkType(r, "value", value, r.ValueType); err != nil {
		return err
	}
	k, err := anypb.New(key)
	if err != nil {
		return &InvalidRequestError{Message: err.Error()}
	}
	v, err := anypb.New(value)
	if err != nil {
		return &InvalidRequestError{Message: err.Error()}
	}
	t.ops = append(t.ops, &pb.TransactionOp{Resource: r.Name, Key: k, Value: v})
	return nil
}

// Erase removes key from r.
func (t *Transaction) Erase(r Resource, key proto.Message) error {
	if err := t.bind(r); err != nil {
		return err
	}
	if err := checkType(r, "key", key, r.KeyType); err != nil {
		return err
	}
	k, err := anypb.New(key)
	if err != nil {
		return &InvalidRequestError{Message: err.Error()}
	}
	t.ops = append(t.ops, &pb.TransactionOp{Resource: r.Name, Erase: true, Key: k})
	return nil
}

func checkType(r Resource, what string, m proto.Message, want string) error {
	if got := string(m.ProtoReflect().Descriptor().FullName()); got != want {
		return &InvalidRequestError{Message: fmt.Sprintf("%s of %s must be %s, not %s", what, r.Name, want, got)}
	}
	return nil
}

// Commit applies every operation or none. A committed transaction returns
// its result again.
func (t *Transaction) Commit(ctx context.Context) (*Applied, error) {
	if t.result != nil {
		return t.result, nil
	}
	if len(t.ops) == 0 {
		return nil, &InvalidRequestError{Message: "empty transaction"}
	}
	req := &pb.ApplyTransactionRequest{
		RequestId:   t.RequestID,
		Ops:         t.ops,
		Consistency: pb.ApplyTransactionRequest_CONSISTENCY_REFERENTIAL,
	}
	if t.snapshot {
		req.Consistency = pb.ApplyTransactionRequest_CONSISTENCY_SCOPE_SNAPSHOT
	}
	if t.hasExpected {
		req.ExpectedGeneration = proto.Uint64(t.expected)
	}
	applied, err := t.c.commit(ctx, req, t.epoch)
	if err == nil {
		t.result = applied
	}
	return applied, err
}

// commit applies req exactly once, whatever the transport does. Every RPC
// counts against the attempt budget. After a send without an answer the
// outcome is asked for; the request is sent again only once GetTransaction
// answered "not known" under the epoch the commit started with.
func (c *Client) commit(ctx context.Context, req *pb.ApplyTransactionRequest, epoch uint64) (*Applied, error) {
	if _, known := c.DaemonEpoch(); !known {
		if _, err := c.Resources(ctx, true); err != nil { // learn the epoch before sending
			return nil, err
		}
	}
	if current, _ := c.DaemonEpoch(); current != epoch {
		return nil, &StaleResourceError{Bound: epoch, Daemon: current}
	}
	backoff := c.retry.BusyBackoff
	var transportFailure, lastUnanswered codes.Code = codes.OK, codes.OK
	unknownAttempt := false // a send of this commit went unanswered
	send := true
	for range c.retry.Attempts {
		if ctx.Err() != nil {
			break
		}
		if send {
			actx, cancel := context.WithTimeout(ctx, c.retry.AttemptTimeout)
			r, md, err := c.t.apply(actx, req)
			cancel()
			if err != nil {
				if !noAnswer(err, md) {
					return nil, translate(err, md)
				}
				transportFailure, lastUnanswered = status.Code(err), status.Code(err)
				unknownAttempt = true
				send = false // ask before sending again
				continue
			}
			if err := c.checkEpoch(r.GetDaemonEpoch(), epoch); err != nil {
				return nil, err
			}
			rec := r.GetRecord()
			if rec.GetOutcome() == pb.TransactionRecord_OUTCOME_BUSY {
				transportFailure = codes.OK
				c.sleep(backoff)
				backoff *= 2
				continue
			}
			return finish(rec, r.GetReplayed(), r.GetDaemonEpoch(), unknownAttempt)
		}
		known, rec, answered, err := c.GetTransaction(ctx, req.GetRequestId())
		if err != nil {
			if te, ok := err.(*TransportError); ok {
				transportFailure, lastUnanswered = te.Code, te.Code
				continue // still no answer: ask again, never resend blind
			}
			// The question was refused, but the unanswered send may have
			// applied: the outcome is unknown, not a refusal.
			code := codes.Unknown
			if ir, ok := err.(*InvalidRequestError); ok {
				code = ir.Code
			}
			return nil, &TransportError{RequestID: req.GetRequestId(), Code: code}
		}
		if err := c.checkEpoch(answered, epoch); err != nil {
			return nil, err
		}
		if known {
			return finish(rec, true, answered, unknownAttempt)
		}
		transportFailure = codes.OK
		send = true // confirmed unseen under this epoch: sending again is safe
	}
	// Out of attempts (or the caller's context ended). After any unanswered
	// send the outcome is unknown: "not known" means "not applied yet", and an
	// unanswered request can still apply later.
	if transportFailure != codes.OK || unknownAttempt {
		code := transportFailure
		if code == codes.OK {
			code = lastUnanswered
		}
		return nil, &TransportError{RequestID: req.GetRequestId(), Code: code}
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return nil, &BusyError{Attempts: c.retry.Attempts}
}

func finish(rec *pb.TransactionRecord, replayed bool, epoch uint64, unknownAttempt bool) (*Applied, error) {
	switch rec.GetOutcome() {
	case pb.TransactionRecord_OUTCOME_APPLIED:
		return &Applied{RequestID: rec.GetRequestId(), Generation: rec.GetGeneration(),
			Visibility: rec.GetVisibility(), Replayed: replayed, DaemonEpoch: epoch, Record: rec}, nil
	case pb.TransactionRecord_OUTCOME_CONFLICT:
		return nil, &ConflictError{Record: rec, AfterUnknownAttempt: unknownAttempt}
	case pb.TransactionRecord_OUTCOME_REJECTED:
		e := &RejectedError{Record: rec}
		for i, op := range rec.GetOps() {
			if op.GetStatus() == pb.TransactionOpResult_STATUS_FAILED {
				e.Failures = append(e.Failures, OpFailure{i, op.GetError()})
			}
		}
		return nil, e
	case pb.TransactionRecord_OUTCOME_BUSY:
		return nil, &BusyError{Attempts: 1}
	}
	return nil, fmt.Errorf("bess: unexpected outcome %v", rec.GetOutcome())
}

// noAnswer: a failed call that says nothing about the outcome: timeouts,
// losses, cancellation, and INTERNAL/UNKNOWN without BESS's error detail (a
// reset stream, a handler that died after recording).
func noAnswer(err error, md metadata.MD) bool {
	switch status.Code(err) {
	case codes.DeadlineExceeded, codes.Unavailable, codes.Canceled:
		return true
	case codes.Internal, codes.Unknown:
		return len(md.Get(errorDetailKey)) == 0
	}
	return false
}

// translate turns a refused call into InvalidRequestError, decoding the
// server's ErrorDetail.
func translate(err error, md metadata.MD) error {
	msg := status.Convert(err).Message()
	var detail *pb.ErrorDetail
	if vals := md.Get(errorDetailKey); len(vals) > 0 {
		d := &pb.ErrorDetail{}
		if proto.Unmarshal([]byte(vals[0]), d) == nil {
			detail = d
			if d.GetMessage() != "" {
				msg = d.GetMessage()
			}
		}
	}
	if detail != nil && detail.GetCode() == pb.ErrorDetail_CONFLICT {
		msg = "request id reused with different contents: " + msg
	}
	return &InvalidRequestError{Message: msg, Code: status.Code(err), Detail: detail}
}
