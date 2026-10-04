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
//
// Desired state: NewPipeline assembles a control_v2 Pipeline (or edits the
// snapshot Client.Pipeline returns); the daemon validates, diffs, plans and
// applies it. ApplyPipeline retries a busy answer and never a conflict
// (PipelineConflictError) or a refusal; with no request id, no answer is a
// TransportError and nothing is resent: read Pipeline again, or apply with
// WithPipelineGeneration so a second application conflicts.
package bess

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"maps"
	"slices"
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

// PipelineConflictError: ApplyPipeline's expected generation no longer
// matched; nothing changed.
type PipelineConflictError struct {
	Message string
	Detail  *pb.ErrorDetail
}

func (e *PipelineConflictError) Error() string { return "bess: pipeline conflict: " + e.Message }

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
	capabilities(ctx context.Context) (*pb.GetCapabilitiesResponse, metadata.MD, error)
	metrics(ctx context.Context) (*pb.ListMetricsResponse, metadata.MD, error)
	watch(ctx context.Context, in *pb.WatchEventsRequest) (eventStream, error)
	// pipeline is one desired-state RPC: in is a Get/Validate/Diff/Plan/
	// ApplyPipelineRequest, the response the matching response.
	pipeline(ctx context.Context, in proto.Message) (proto.Message, metadata.MD, error)
}

// eventStream is what WatchEvents reads (the gRPC client stream, or a test's).
type eventStream interface {
	Recv() (*pb.Event, error)
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

func (t grpcTransport) capabilities(ctx context.Context) (*pb.GetCapabilitiesResponse, metadata.MD, error) {
	var md metadata.MD
	r, err := t.c.GetCapabilities(ctx, &pb.GetCapabilitiesRequest{}, grpc.Trailer(&md))
	return r, md, err
}

func (t grpcTransport) metrics(ctx context.Context) (*pb.ListMetricsResponse, metadata.MD, error) {
	var md metadata.MD
	r, err := t.c.ListMetrics(ctx, &pb.ListMetricsRequest{}, grpc.Trailer(&md))
	return r, md, err
}

func (t grpcTransport) watch(ctx context.Context, in *pb.WatchEventsRequest) (eventStream, error) {
	return t.c.WatchEvents(ctx, in)
}

func (t grpcTransport) pipeline(ctx context.Context, in proto.Message) (proto.Message, metadata.MD, error) {
	var md metadata.MD
	var r proto.Message
	var err error
	switch req := in.(type) {
	case *pb.GetPipelineRequest:
		r, err = t.c.GetPipeline(ctx, req, grpc.Trailer(&md))
	case *pb.ValidatePipelineRequest:
		r, err = t.c.ValidatePipeline(ctx, req, grpc.Trailer(&md))
	case *pb.DiffPipelineRequest:
		r, err = t.c.DiffPipeline(ctx, req, grpc.Trailer(&md))
	case *pb.PlanPipelineRequest:
		r, err = t.c.PlanPipeline(ctx, req, grpc.Trailer(&md))
	case *pb.ApplyPipelineRequest:
		r, err = t.c.ApplyPipeline(ctx, req, grpc.Trailer(&md))
	default:
		return nil, nil, fmt.Errorf("bess: not a pipeline request: %T", in)
	}
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

// Capabilities is what the daemon offers (control_v2.GetCapabilities):
// version, RPCs, plugin API and granted capabilities, module classes,
// plugins, ports and resources. A daemon too old to serve it answers
// InvalidRequestError (UNIMPLEMENTED).
func (c *Client) Capabilities(ctx context.Context) (*pb.GetCapabilitiesResponse, error) {
	actx, cancel := context.WithTimeout(ctx, c.retry.AttemptTimeout)
	defer cancel()
	r, md, err := c.t.capabilities(actx)
	if err != nil {
		if noAnswer(err, md) {
			return nil, &TransportError{Code: status.Code(err)}
		}
		return nil, translate(err, md)
	}
	c.observeEpoch(r.GetDaemonEpoch())
	return r, nil
}

// Supports reports whether the daemon serves rpc ("ApplyTransaction" or a
// full name). A daemon too old to answer GetCapabilities (UNIMPLEMENTED)
// serves none of the newer RPCs; any other failure is returned.
func (c *Client) Supports(ctx context.Context, rpc string) (bool, error) {
	r, err := c.Capabilities(ctx)
	if err != nil {
		var invalid *InvalidRequestError
		if errors.As(err, &invalid) && invalid.Code == codes.Unimplemented {
			return false, nil
		}
		return false, err
	}
	for _, name := range r.GetRpcs() {
		if name == rpc || strings.HasSuffix(name, "/"+rpc) {
			return true, nil
		}
	}
	return false, nil
}

// WatchEvents streams operational events (control_v2.WatchEvents), oldest
// first, from `from` (0: the next event); to resume from a saved sequence,
// pass the epoch it belongs to (0: unknown). With types, only those (gaps
// always). A "bess.gap" event names events the daemon no longer held. After a
// transport failure the stream resumes where it stopped, even before its
// first event. The event channel closes when ctx ends or on an error; then
// the error channel yields the error, if any, and closes: DaemonRestartedError
// when the daemon restarted (what was built from earlier events is gone:
// re-read the state, watch from 0). "bess.start" and "bess.progress" are not
// delivered.
func (c *Client) WatchEvents(ctx context.Context, from, epoch uint64, types ...string) (<-chan *pb.Event, <-chan error) {
	events := make(chan *pb.Event)
	errc := make(chan error, 1)
	go func() {
		defer close(errc)
		defer close(events)
		next := from
		backoff := c.retry.BusyBackoff
		for ctx.Err() == nil {
			stream, err := c.t.watch(ctx, &pb.WatchEventsRequest{FromSequence: next, Types: types, DaemonEpoch: epoch})
			for err == nil {
				var e *pb.Event
				if e, err = stream.Recv(); err != nil {
					break
				}
				if e.GetType() == "bess.restart" || (epoch != 0 && e.GetDaemonEpoch() != epoch) {
					c.observeEpoch(e.GetDaemonEpoch())
					errc <- &DaemonRestartedError{From: epoch, To: e.GetDaemonEpoch()}
					return
				}
				epoch = e.GetDaemonEpoch()
				switch e.GetType() {
				case "bess.start":
					next = e.GetSequence()
					continue
				case "bess.progress":
					next = e.GetSequence() + 1
					continue
				case "bess.gap":
					next = e.GetGapTo()
				default:
					next = e.GetSequence() + 1
				}
				select {
				case events <- e:
				case <-ctx.Done():
					return
				}
			}
			if ctx.Err() != nil {
				return
			}
			if !noAnswer(err, nil) {
				errc <- translate(err, nil)
				return
			}
			c.sleep(backoff)
			backoff = min(backoff*2, time.Second)
		}
	}()
	return events, errc
}

// Metrics is every metric sample (control_v2.ListMetrics).
func (c *Client) Metrics(ctx context.Context) ([]*pb.MetricSample, error) {
	actx, cancel := context.WithTimeout(ctx, c.retry.AttemptTimeout)
	defer cancel()
	r, md, err := c.t.metrics(actx)
	if err != nil {
		if noAnswer(err, md) {
			return nil, &TransportError{Code: status.Code(err)}
		}
		return nil, translate(err, md)
	}
	c.observeEpoch(r.GetDaemonEpoch())
	return r.GetSamples(), nil
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

// -- desired state ----------------------------------------------------------------

// PipelineSnapshot is the active pipeline and its generation.
type PipelineSnapshot struct {
	Pipeline   *pb.Pipeline
	Generation uint64
}

// Pipeline is the active pipeline as desired state.
func (c *Client) Pipeline(ctx context.Context) (PipelineSnapshot, error) {
	r, err := c.pipelineCall(ctx, &pb.GetPipelineRequest{}, false)
	if err != nil {
		return PipelineSnapshot{}, err
	}
	g := r.(*pb.GetPipelineResponse)
	return PipelineSnapshot{g.GetPipeline(), g.GetGeneration()}, nil
}

// ValidatePipeline is the daemon's canonical form of p (InvalidRequestError
// if p is invalid).
func (c *Client) ValidatePipeline(ctx context.Context, p *pb.Pipeline) (*pb.Pipeline, error) {
	r, err := c.pipelineCall(ctx, &pb.ValidatePipelineRequest{Pipeline: p}, false)
	if err != nil {
		return nil, err
	}
	return r.(*pb.ValidatePipelineResponse).GetNormalized(), nil
}

// DiffPipeline is what applying p would change, and the generation it was
// taken against.
func (c *Client) DiffPipeline(ctx context.Context, p *pb.Pipeline) (*pb.PipelineDiff, uint64, error) {
	r, err := c.pipelineCall(ctx, &pb.DiffPipelineRequest{Pipeline: p}, false)
	if err != nil {
		return nil, 0, err
	}
	d := r.(*pb.DiffPipelineResponse)
	return d.GetDiff(), d.GetGeneration(), nil
}

// PlanPipeline is the daemon's plan for p, and the generation it was made
// against.
func (c *Client) PlanPipeline(ctx context.Context, p *pb.Pipeline) ([]*pb.PlanStep, uint64, error) {
	r, err := c.pipelineCall(ctx, &pb.PlanPipelineRequest{Pipeline: p}, false)
	if err != nil {
		return nil, 0, err
	}
	pl := r.(*pb.PlanPipelineResponse)
	return pl.GetSteps(), pl.GetGeneration(), nil
}

// PipelineOption sets an ApplyPipeline option.
type PipelineOption func(*pb.ApplyPipelineRequest)

// WithPipelineGeneration applies only if the active generation is g
// (PipelineConflictError otherwise).
func WithPipelineGeneration(g uint64) PipelineOption {
	return func(r *pb.ApplyPipelineRequest) { r.ExpectedGeneration = &g }
}

// ApplyPipeline makes p the active pipeline, all or nothing.
func (c *Client) ApplyPipeline(ctx context.Context, p *pb.Pipeline, opts ...PipelineOption) (*pb.ApplyPipelineResponse, error) {
	req := &pb.ApplyPipelineRequest{Pipeline: p}
	for _, o := range opts {
		o(req)
	}
	r, err := c.pipelineCall(ctx, req, true)
	if err != nil {
		return nil, err
	}
	return r.(*pb.ApplyPipelineResponse), nil
}

// pipelineCall runs a desired-state RPC. An answered refusal carries an
// ErrorDetail: CONFLICT is PipelineConflictError, RESOURCE_BUSY is retried
// (retryBusy) within the policy; no answer is TransportError.
func (c *Client) pipelineCall(ctx context.Context, in proto.Message, retryBusy bool) (proto.Message, error) {
	backoff := c.retry.BusyBackoff
	for attempt := 1; ; attempt++ {
		actx, cancel := context.WithTimeout(ctx, c.retry.AttemptTimeout)
		r, md, err := c.t.pipeline(actx, in)
		cancel()
		if err == nil {
			return r, nil
		}
		detail := errorDetail(md)
		if detail == nil && noAnswer(err, md) {
			return nil, &TransportError{Code: status.Code(err)}
		}
		switch detail.GetCode() {
		case pb.ErrorDetail_CONFLICT:
			return nil, &PipelineConflictError{Message: detail.GetMessage(), Detail: detail}
		case pb.ErrorDetail_RESOURCE_BUSY:
			if retryBusy && attempt < c.retry.Attempts && ctx.Err() == nil {
				c.sleep(backoff)
				backoff *= 2
				continue
			}
			return nil, &BusyError{Attempts: attempt}
		}
		return nil, translate(err, md)
	}
}

// PipelineBuilder assembles a control_v2 Pipeline. It only builds the
// message: the daemon validates, diffs and plans it.
type PipelineBuilder struct {
	p   *pb.Pipeline
	err error
}

// NewPipeline starts from base (a snapshot's pipeline, copied) or empty (nil).
func NewPipeline(base *pb.Pipeline) *PipelineBuilder {
	if base == nil {
		return &PipelineBuilder{p: &pb.Pipeline{}}
	}
	return &PipelineBuilder{p: proto.Clone(base).(*pb.Pipeline)}
}

func (b *PipelineBuilder) pack(arg proto.Message) *anypb.Any {
	if arg == nil || b.err != nil {
		return nil
	}
	a, err := anypb.New(arg)
	if err != nil {
		b.err = err
	}
	return a
}

// Port adds a port (queue counts and sizes as in pb.Port: PortSpec for those).
func (b *PipelineBuilder) Port(name, driver string, arg proto.Message) *PipelineBuilder {
	return b.PortSpec(&pb.Port{Name: name, Driver: driver, Arg: b.pack(arg)})
}

// PortSpec adds a port as given.
func (b *PipelineBuilder) PortSpec(p *pb.Port) *PipelineBuilder {
	b.p.Ports = append(b.p.Ports, p)
	return b
}

// Module adds a module with its Init argument (nil: none).
func (b *PipelineBuilder) Module(name, mclass string, arg proto.Message) *PipelineBuilder {
	b.p.Modules = append(b.p.Modules, &pb.Module{Name: name, Mclass: mclass, Arg: b.pack(arg)})
	return b
}

// Connect adds upstream:ogate -> igate:downstream.
func (b *PipelineBuilder) Connect(upstream string, ogate uint32, downstream string, igate uint32) *PipelineBuilder {
	b.p.Connections = append(b.p.Connections,
		&pb.Connection{Upstream: upstream, Ogate: ogate, Downstream: downstream, Igate: igate})
	return b
}

// Chain connects gate 0 to gate 0 along names.
func (b *PipelineBuilder) Chain(names ...string) *PipelineBuilder {
	for i := 1; i < len(names); i++ {
		b.Connect(names[i-1], 0, names[i], 0)
	}
	return b
}

// Worker adds a worker.
func (b *PipelineBuilder) Worker(wid, core int32, scheduler string) *PipelineBuilder {
	b.p.Workers = append(b.p.Workers, &pb.Worker{Wid: wid, Core: core, Scheduler: scheduler})
	return b
}

// TrafficClass adds a traffic class as given.
func (b *PipelineBuilder) TrafficClass(tc *pb.TrafficClass) *PipelineBuilder {
	b.p.TrafficClasses = append(b.p.TrafficClasses, tc)
	return b
}

// Remove drops the port, module or traffic class name and the connections
// that touch it.
func (b *PipelineBuilder) Remove(name string) *PipelineBuilder {
	b.p.Ports = slices.DeleteFunc(b.p.Ports, func(p *pb.Port) bool { return p.GetName() == name })
	b.p.Modules = slices.DeleteFunc(b.p.Modules, func(m *pb.Module) bool { return m.GetName() == name })
	b.p.TrafficClasses = slices.DeleteFunc(b.p.TrafficClasses,
		func(tc *pb.TrafficClass) bool { return tc.GetName() == name })
	b.p.Connections = slices.DeleteFunc(b.p.Connections, func(c *pb.Connection) bool {
		return c.GetUpstream() == name || c.GetDownstream() == name
	})
	return b
}

// Build is a copy of the pipeline built so far, or the first argument that
// could not be packed.
func (b *PipelineBuilder) Build() (*pb.Pipeline, error) {
	if b.err != nil {
		return nil, b.err
	}
	return proto.Clone(b.p).(*pb.Pipeline), nil
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

// errorDetail decodes the server's ErrorDetail from a failed call's
// trailers (nil: none).
func errorDetail(md metadata.MD) *pb.ErrorDetail {
	if vals := md.Get(errorDetailKey); len(vals) > 0 {
		d := &pb.ErrorDetail{}
		if proto.Unmarshal([]byte(vals[0]), d) == nil {
			return d
		}
	}
	return nil
}

// translate turns a refused call into InvalidRequestError, decoding the
// server's ErrorDetail.
func translate(err error, md metadata.MD) error {
	msg := status.Convert(err).Message()
	detail := errorDetail(md)
	if detail.GetMessage() != "" {
		msg = detail.GetMessage()
	}
	if detail != nil && detail.GetCode() == pb.ErrorDetail_CONFLICT {
		msg = "request id reused with different contents: " + msg
	}
	return &InvalidRequestError{Message: msg, Code: status.Code(err), Detail: detail}
}
