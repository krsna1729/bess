// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"context"
	"errors"
	"fmt"
	"slices"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/anypb"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// Two control messages stand in for a resource's key and value types.
var (
	keyType   = string((&pb.GetTransactionRequest{}).ProtoReflect().Descriptor().FullName())
	valueType = string((&pb.ListTransactionResourcesRequest{}).ProtoReflect().Descriptor().FullName())
)

const resource = "m0/table"

// step is one scripted answer: a response (outcome/known with epoch) or an
// error with optional trailers.
type step struct {
	outcome pb.TransactionRecord_Outcome
	known   bool
	epoch   uint64
	err     error
	md      metadata.MD
}

func timeout() step                                            { return step{err: status.Error(codes.DeadlineExceeded, "deadline")} }
func unavailable() step                                        { return step{err: status.Error(codes.Unavailable, "down")} }
func answer(o pb.TransactionRecord_Outcome, epoch uint64) step { return step{outcome: o, epoch: epoch} }
func notKnown(epoch uint64) step                               { return step{known: false, epoch: epoch} }
func known(o pb.TransactionRecord_Outcome, epoch uint64) step {
	return step{known: true, outcome: o, epoch: epoch}
}

type fake struct {
	epoch     uint64
	applies   []step
	gets      []step
	sent      []*pb.ApplyTransactionRequest
	asked     []string
	listed    int
	rejectOps bool
	oldDaemon bool // answers GetCapabilities with UNIMPLEMENTED
	refuse    bool // answers GetCapabilities with PERMISSION_DENIED
	streams   []*scriptedStream
	watched   [][2]uint64 // (from_sequence, daemon_epoch) per WatchEvents call
	pipelines []step      // ApplyPipeline's script (err/md; nil err: applied)
	applied   []*pb.ApplyPipelineRequest
	captures  []*pb.SetCaptureRequest
	reads     []*pb.GetPipelineRequest  // GetPipeline requests
	actions   []step                    // InvokeAction's script (err/md; nil err: answered)
	invoked   []*pb.InvokeActionRequest // InvokeAction requests
}

// actionResult is what the fake's actions answer.
var actionResult = &pb.GetTransactionRequest{RequestId: "answer"}

// singleton is the fake's singleton resource; it requires paused workers.
const singleton = "m0/settings"

func (f *fake) admin(_ context.Context, in proto.Message) (proto.Message, metadata.MD, error) {
	switch req := in.(type) {
	case *pb.SetCaptureRequest:
		f.captures = append(f.captures, proto.Clone(req).(*pb.SetCaptureRequest))
		if req.GetDaemonEpoch() != f.epochOr1() {
			md, err := epochRefusal(req.GetDaemonEpoch(), f.epochOr1())
			return nil, md, err
		}
		return &pb.SetCaptureResponse{DaemonEpoch: f.epochOr1()}, nil, nil
	case *pb.InvokeActionRequest:
		f.invoked = append(f.invoked, proto.Clone(req).(*pb.InvokeActionRequest))
		if req.GetDaemonEpoch() != f.epochOr1() {
			md, err := epochRefusal(req.GetDaemonEpoch(), f.epochOr1())
			return nil, md, err
		}
		if len(f.actions) > 0 {
			s := f.actions[0]
			f.actions = f.actions[1:]
			if s.err != nil {
				return nil, s.md, s.err
			}
		}
		result, _ := anypb.New(actionResult)
		return &pb.InvokeActionResponse{Result: result, DaemonEpoch: f.epochOr1()}, nil, nil
	}
	return nil, nil, status.Error(codes.Unimplemented, "fake")
}

func (f *fake) pipeline(_ context.Context, in proto.Message) (proto.Message, metadata.MD, error) {
	switch req := in.(type) {
	case *pb.GetPipelineRequest:
		f.reads = append(f.reads, req)
		return &pb.GetPipelineResponse{Pipeline: &pb.Pipeline{}, Generation: 3, DaemonEpoch: f.epochOr1(),
			ResourceGeneration: 9}, nil, nil
	case *pb.DiffPipelineRequest:
		return &pb.DiffPipelineResponse{Diff: &pb.PipelineDiff{}, Generation: 3, DaemonEpoch: f.epochOr1(),
			ResourceGeneration: 9}, nil, nil
	case *pb.PlanPipelineRequest:
		return &pb.PlanPipelineResponse{Generation: 3, DaemonEpoch: f.epochOr1(), ResourceGeneration: 9}, nil, nil
	case *pb.ApplyPipelineRequest:
		f.applied = append(f.applied, req)
		if req.GetDaemonEpoch() != f.epochOr1() {
			md, err := epochRefusal(req.GetDaemonEpoch(), f.epochOr1())
			return nil, md, err
		}
		if len(f.pipelines) > 0 {
			s := f.pipelines[0]
			f.pipelines = f.pipelines[1:]
			if s.err != nil {
				return nil, s.md, s.err
			}
		}
		return &pb.ApplyPipelineResponse{Generation: 4, DaemonEpoch: f.epochOr1()}, nil, nil
	}
	return nil, nil, status.Error(codes.Unimplemented, "fake")
}

func refusal(code codes.Code, detail pb.ErrorDetail_Code) step {
	d, _ := proto.Marshal(&pb.ErrorDetail{Code: detail, Message: "refused"})
	return step{err: status.Error(code, "x"), md: metadata.Pairs(errorDetailKey, string(d))}
}

// epochRefusal is what bessd answers a mutating request built under another
// epoch (D-106).
func epochRefusal(sent, daemon uint64) (metadata.MD, error) {
	if sent == 0 {
		d, _ := proto.Marshal(&pb.ErrorDetail{Code: pb.ErrorDetail_INVALID_ARGUMENT, Field: "daemon_epoch"})
		return metadata.Pairs(errorDetailKey, string(d)), status.Error(codes.InvalidArgument, "no epoch")
	}
	d, _ := proto.Marshal(&pb.ErrorDetail{Code: pb.ErrorDetail_EPOCH_MISMATCH, Field: "daemon_epoch",
		DaemonEpoch: daemon, Message: "built under another epoch"})
	return metadata.Pairs(errorDetailKey, string(d)), status.Error(codes.FailedPrecondition, "epoch")
}

func (f *fake) apply(_ context.Context, in *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	f.sent = append(f.sent, proto.Clone(in).(*pb.ApplyTransactionRequest))
	s := f.applies[0]
	f.applies = f.applies[1:]
	if s.err != nil {
		return nil, s.md, s.err
	}
	if in.GetDaemonEpoch() != s.epoch { // the step's epoch is the daemon's
		md, err := epochRefusal(in.GetDaemonEpoch(), s.epoch)
		return nil, md, err
	}
	rec := &pb.TransactionRecord{RequestId: in.GetRequestId(), Outcome: s.outcome, Generation: 7}
	if f.rejectOps {
		rec.Ops = []*pb.TransactionOpResult{{Status: pb.TransactionOpResult_STATUS_FAILED, Error: "no such next hop"}}
	}
	return &pb.ApplyTransactionResponse{Record: rec, DaemonEpoch: s.epoch}, nil, nil
}

func (f *fake) get(_ context.Context, in *pb.GetTransactionRequest) (*pb.GetTransactionResponse, metadata.MD, error) {
	f.asked = append(f.asked, in.GetRequestId())
	s := f.gets[0]
	f.gets = f.gets[1:]
	if s.err != nil {
		return nil, s.md, s.err
	}
	return &pb.GetTransactionResponse{Known: s.known, DaemonEpoch: s.epoch,
		Record: &pb.TransactionRecord{RequestId: in.GetRequestId(), Outcome: s.outcome, Generation: 7}}, nil, nil
}

func (f *fake) list(context.Context) (*pb.ListTransactionResourcesResponse, metadata.MD, error) {
	f.listed++
	epoch := f.epoch
	if epoch == 0 {
		epoch = 1
	}
	// resource has no kind (a daemon before D-129): a map.
	return &pb.ListTransactionResourcesResponse{DaemonEpoch: epoch, Resources: []*pb.TransactionResource{
		{Name: resource, KeyType: keyType, ValueType: valueType},
		{Name: singleton, KeyType: "google.protobuf.Empty", ValueType: valueType,
			Kind: pb.TransactionResource_KIND_SINGLETON, RequiresPausedWorkers: true}}}, nil, nil
}

func (f *fake) capabilities(context.Context) (*pb.GetCapabilitiesResponse, metadata.MD, error) {
	if f.oldDaemon {
		return nil, nil, status.Error(codes.Unimplemented, "unknown method GetCapabilities")
	}
	if f.refuse {
		return nil, nil, status.Error(codes.PermissionDenied, "no")
	}
	return &pb.GetCapabilitiesResponse{Rpcs: []string{"bess.pb.v2.Control/ApplyTransaction"},
		DaemonEpoch: f.epochOr1()}, nil, nil
}

func (f *fake) schema(context.Context) (*pb.GetSchemaResponse, metadata.MD, error) {
	return &pb.GetSchemaResponse{DaemonEpoch: f.epochOr1()}, nil, nil
}

func (f *fake) metrics(context.Context) (*pb.ListMetricsResponse, metadata.MD, error) {
	return &pb.ListMetricsResponse{DaemonEpoch: f.epochOr1()}, nil, nil
}

func (f *fake) epochOr1() uint64 {
	if f.epoch == 0 {
		return 1
	}
	return f.epoch
}

type scriptedStream struct{ steps []any }

func (s *scriptedStream) Recv() (*pb.Event, error) {
	if len(s.steps) == 0 {
		return nil, status.Error(codes.Unavailable, "end of script")
	}
	step := s.steps[0]
	s.steps = s.steps[1:]
	if err, ok := step.(error); ok {
		return nil, err
	}
	return step.(*pb.Event), nil
}

func (f *fake) watch(_ context.Context, in *pb.WatchEventsRequest) (eventStream, error) {
	f.watched = append(f.watched, [2]uint64{in.GetFromSequence(), in.GetDaemonEpoch()})
	if len(f.streams) == 0 {
		return nil, status.Error(codes.Unavailable, "no more streams")
	}
	st := f.streams[0]
	f.streams = f.streams[1:]
	return st, nil
}

type harness struct {
	f      *fake
	c      *Client
	sleeps []time.Duration
}

func newHarness(f *fake, attempts int) *harness {
	h := &harness{f: f}
	h.c = newClient(f, RetryPolicy{AttemptTimeout: time.Second, Attempts: attempts,
		BusyBackoff: 10 * time.Millisecond}, func(d time.Duration) { h.sleeps = append(h.sleeps, d) })
	return h
}

func (h *harness) commit(t *testing.T, opts ...TxOption) (*Applied, error) {
	t.Helper()
	ctx := context.Background()
	r, err := h.c.Resource(ctx, resource)
	if err != nil {
		t.Fatal(err)
	}
	tx := h.c.Transaction(opts...)
	if err := tx.Upsert(r, &pb.GetTransactionRequest{RequestId: "k"}, &pb.ListTransactionResourcesRequest{}); err != nil {
		t.Fatal(err)
	}
	return tx.Commit(ctx)
}

func as[T error](t *testing.T, err error) T {
	t.Helper()
	var target T
	if !errors.As(err, &target) {
		t.Fatalf("got %v (%T), want %T", err, err, target)
	}
	return target
}

const (
	applied   = pb.TransactionRecord_OUTCOME_APPLIED
	rejected  = pb.TransactionRecord_OUTCOME_REJECTED
	conflict  = pb.TransactionRecord_OUTCOME_CONFLICT
	busy      = pb.TransactionRecord_OUTCOME_BUSY
	noOutcome = pb.TransactionRecord_OUTCOME_UNSPECIFIED
)

func TestTypesAreCheckedBeforeAnythingIsSent(t *testing.T) {
	h := newHarness(&fake{}, 4)
	r, _ := h.c.Resource(context.Background(), resource)
	tx := h.c.Transaction()
	as[*InvalidRequestError](t, tx.Upsert(r, &pb.ListTransactionResourcesRequest{}, &pb.ListTransactionResourcesRequest{}))
	as[*InvalidRequestError](t, tx.Upsert(r, &pb.GetTransactionRequest{}, &pb.GetTransactionRequest{}))
	_, err := h.c.Resource(context.Background(), "missing/table")
	as[*InvalidRequestError](t, err)
	if len(h.f.sent) != 0 {
		t.Fatal("sent something")
	}
}

func TestApplied(t *testing.T) {
	h := newHarness(&fake{applies: []step{answer(applied, 1)}}, 4)
	a, err := h.commit(t)
	if err != nil || a.Generation != 7 || a.Replayed || len(h.f.sent) != 1 || a.RequestID == "" {
		t.Fatalf("%+v %v", a, err)
	}
}

func TestATimeoutIsResolvedByAskingNotByResending(t *testing.T) {
	h := newHarness(&fake{applies: []step{timeout()}, gets: []step{known(applied, 1)}}, 4)
	a, err := h.commit(t)
	if err != nil || !a.Replayed || len(h.f.sent) != 1 || h.f.asked[0] != h.f.sent[0].GetRequestId() {
		t.Fatalf("%+v %v", a, err)
	}
}

func TestAnUnseenRequestIsResentIdentically(t *testing.T) {
	h := newHarness(&fake{applies: []step{timeout(), answer(applied, 1)}, gets: []step{notKnown(1)}}, 4)
	if _, err := h.commit(t); err != nil {
		t.Fatal(err)
	}
	if len(h.f.sent) != 2 || !proto.Equal(h.f.sent[0], h.f.sent[1]) {
		t.Fatal("not resent identically")
	}
}

func TestAnUnansweredQuestionIsAskedAgainNotAnsweredByResending(t *testing.T) {
	h := newHarness(&fake{applies: []step{unavailable(), answer(applied, 1)},
		gets: []step{unavailable(), notKnown(1)}}, 4)
	if _, err := h.commit(t); err != nil {
		t.Fatal(err)
	}
	if len(h.f.asked) != 2 || len(h.f.sent) != 2 || h.f.sent[0].GetRequestId() != h.f.sent[1].GetRequestId() {
		t.Fatalf("asked %d sent %d", len(h.f.asked), len(h.f.sent))
	}
}

func TestARestartBehindAFailedQuestionIsNotAppliedBlind(t *testing.T) {
	h := newHarness(&fake{applies: []step{timeout()}, gets: []step{unavailable(), notKnown(2)}}, 4)
	_, err := h.commit(t)
	as[*DaemonRestartedError](t, err)
	if len(h.f.sent) != 1 {
		t.Fatal("resent to the restarted daemon")
	}
}

func TestARestartDuringRecoveryIsReportedNotGuessed(t *testing.T) {
	h := newHarness(&fake{applies: []step{timeout()}, gets: []step{notKnown(2)}}, 4)
	_, err := h.commit(t)
	as[*DaemonRestartedError](t, err)
}

func TestTheEpochIsLearnedBeforeTheFirstSend(t *testing.T) {
	h := newHarness(&fake{applies: []step{answer(applied, 2)}}, 4)
	tx := h.c.Transaction()
	r := Resource{Name: resource, KeyType: keyType, ValueType: valueType, DaemonEpoch: 1} // no discovery through this client
	_ = tx.Upsert(r, &pb.GetTransactionRequest{}, &pb.ListTransactionResourcesRequest{})
	_, err := tx.Commit(context.Background())
	as[*DaemonRestartedError](t, err)
	if h.f.listed != 1 {
		t.Fatal("epoch not learned first")
	}
}

func upsert(tx *Transaction, r Resource) error {
	return tx.Upsert(r, &pb.GetTransactionRequest{}, &pb.ListTransactionResourcesRequest{})
}

func TestAHandleFromBeforeARestartIsRefusedNotSent(t *testing.T) {
	ctx := context.Background()
	h := newHarness(&fake{epoch: 1, applies: []step{answer(applied, 2)}}, 4)
	old, _ := h.c.Resource(ctx, resource)
	built := h.c.Transaction()
	if err := upsert(built, old); err != nil {
		t.Fatal(err)
	}
	h.f.epoch = 2
	if _, err := h.c.Resources(ctx, true); err != nil { // the client sees the restart
		t.Fatal(err)
	}
	as[*StaleResourceError](t, upsert(h.c.Transaction(), old))
	_, err := built.Commit(ctx) // built before the restart: never sent
	as[*StaleResourceError](t, err)
	if len(h.f.sent) != 0 {
		t.Fatal("sent with a stale handle")
	}
	fresh, _ := h.c.Resource(ctx, resource) // looked up again: usable
	tx := h.c.Transaction()
	if err := upsert(tx, fresh); err != nil || fresh.DaemonEpoch != 2 {
		t.Fatal(err, fresh)
	}
	if _, err := tx.Commit(ctx); err != nil || len(h.f.sent) != 1 {
		t.Fatal(err, len(h.f.sent))
	}
}

func TestARestartSeenByACommitInvalidatesTheOldHandles(t *testing.T) {
	ctx := context.Background()
	h := newHarness(&fake{epoch: 1, applies: []step{answer(applied, 2)}}, 4)
	old, _ := h.c.Resource(ctx, resource)
	tx := h.c.Transaction()
	_ = upsert(tx, old)
	_, err := tx.Commit(ctx)
	// It named its resources' epoch; the daemon refused it (D-106): an
	// answer, so nothing to ask and nothing applied.
	if e := as[*DaemonRestartedError](t, err); !e.Refused || e.From != 1 || e.To != 2 {
		t.Fatal(e)
	}
	if h.f.sent[0].GetDaemonEpoch() != 1 || len(h.f.sent) != 1 || len(h.f.asked) != 0 {
		t.Fatal(h.f.sent, h.f.asked)
	}
	as[*StaleResourceError](t, upsert(h.c.Transaction(), old))
}

func TestOneTransactionHoldsHandlesOfOneEpoch(t *testing.T) {
	ctx := context.Background()
	h := newHarness(&fake{epoch: 1}, 4)
	old, _ := h.c.Resource(ctx, resource)
	tx := h.c.Transaction()
	_ = upsert(tx, old)
	h.f.epoch = 2
	all, _ := h.c.Resources(ctx, true)                    // the client sees the restart
	as[*StaleResourceError](t, upsert(tx, all[resource])) // valid now, but the transaction is not
	as[*StaleResourceError](t, tx.Erase(all[resource], &pb.GetTransactionRequest{}))
}

func TestInternalWithoutBessDetailIsNoAnswer(t *testing.T) {
	h := newHarness(&fake{applies: []step{{err: status.Error(codes.Internal, "stream reset")}},
		gets: []step{known(applied, 1)}}, 4)
	a, err := h.commit(t)
	if err != nil || !a.Replayed {
		t.Fatal(a, err)
	}
	d, _ := proto.Marshal(&pb.ErrorDetail{Code: pb.ErrorDetail_INTERNAL, Message: "engine failure"})
	h = newHarness(&fake{applies: []step{{err: status.Error(codes.Internal, "x"),
		md: metadata.Pairs(errorDetailKey, string(d))}}}, 4)
	_, err = h.commit(t)
	as[*InvalidRequestError](t, err)
}

func TestServerErrorsCarryTheirDetail(t *testing.T) {
	d, _ := proto.Marshal(&pb.ErrorDetail{Code: pb.ErrorDetail_INVALID_ARGUMENT, Message: "bad key", Field: "key"})
	h := newHarness(&fake{applies: []step{{err: status.Error(codes.InvalidArgument, "x"),
		md: metadata.Pairs(errorDetailKey, string(d))}}}, 4)
	_, err := h.commit(t)
	e := as[*InvalidRequestError](t, err)
	if e.Detail.GetField() != "key" || e.Message != "bad key" {
		t.Fatal(e)
	}
}

func TestBusyIsRetriedWithBackoff(t *testing.T) {
	h := newHarness(&fake{applies: []step{answer(busy, 1), answer(busy, 1), answer(applied, 1)}}, 4)
	if _, err := h.commit(t); err != nil {
		t.Fatal(err)
	}
	if len(h.sleeps) != 2 || h.sleeps[0] != 10*time.Millisecond || h.sleeps[1] != 20*time.Millisecond {
		t.Fatal(h.sleeps)
	}
}

func TestBusyThroughEveryAttempt(t *testing.T) {
	h := newHarness(&fake{applies: []step{answer(busy, 1), answer(busy, 1), answer(busy, 1)}}, 3)
	_, err := h.commit(t)
	as[*BusyError](t, err)
}

func TestOutOfAttemptsAfterAnUnansweredSendIsUnknownNotBusy(t *testing.T) {
	for i, f := range []*fake{
		{applies: []step{timeout()}, gets: []step{notKnown(1)}},
		{applies: []step{timeout(), answer(busy, 1)}, gets: []step{notKnown(1)}},
		{applies: []step{timeout(), timeout()}, gets: []step{notKnown(1), notKnown(1)}},
	} {
		h := newHarness(f, []int{2, 3, 4}[i])
		_, err := h.commit(t)
		as[*TransportError](t, err)
	}
}

func TestNoAnswerAtAllIsAnUnknownOutcome(t *testing.T) {
	h := newHarness(&fake{applies: []step{timeout()}, gets: []step{timeout()}}, 2)
	_, err := h.commit(t)
	e := as[*TransportError](t, err)
	if e.RequestID != h.f.sent[0].GetRequestId() {
		t.Fatal(e)
	}
}

func TestARefusedQuestionAfterAnUnansweredSendIsUnknown(t *testing.T) {
	h := newHarness(&fake{applies: []step{timeout()},
		gets: []step{{err: status.Error(codes.PermissionDenied, "proxy")}}}, 4)
	_, err := h.commit(t)
	if e := as[*TransportError](t, err); e.Code != codes.PermissionDenied {
		t.Fatal(e)
	}
}

func TestResourcesReturnsACopy(t *testing.T) {
	h := newHarness(&fake{}, 4)
	all, _ := h.c.Resources(context.Background(), false)
	delete(all, resource)
	if _, err := h.c.Resource(context.Background(), resource); err != nil {
		t.Fatal("editing the returned map changed the client's cache")
	}
}

func TestConflictAndRejection(t *testing.T) {
	h := newHarness(&fake{applies: []step{answer(conflict, 1)}}, 4)
	_, err := h.commit(t, WithExpectedGeneration(3))
	if as[*ConflictError](t, err).AfterUnknownAttempt || h.f.sent[0].GetExpectedGeneration() != 3 {
		t.Fatal(err)
	}
	h = newHarness(&fake{applies: []step{timeout(), answer(conflict, 1)}, gets: []step{notKnown(1)}}, 4)
	_, err = h.commit(t, WithExpectedGeneration(3))
	if !as[*ConflictError](t, err).AfterUnknownAttempt {
		t.Fatal("conflict after an unanswered send must say it may have applied")
	}
	h = newHarness(&fake{applies: []step{answer(rejected, 1)}, rejectOps: true}, 4)
	_, err = h.commit(t)
	e := as[*RejectedError](t, err)
	if len(e.Failures) != 1 || e.Failures[0].Error != "no such next hop" || len(h.f.sent) != 1 {
		t.Fatal(e)
	}
}

func TestACommittedTransactionIsNotSentAgain(t *testing.T) {
	h := newHarness(&fake{applies: []step{answer(applied, 1)}}, 4)
	r, _ := h.c.Resource(context.Background(), resource)
	tx := h.c.Transaction()
	_ = tx.Upsert(r, &pb.GetTransactionRequest{}, &pb.ListTransactionResourcesRequest{})
	a1, _ := tx.Commit(context.Background())
	a2, err := tx.Commit(context.Background())
	if err != nil || a1 != a2 || len(h.f.sent) != 1 {
		t.Fatal(err)
	}
	_ = noOutcome
}

func TestSupportsReadsTheDaemonsRPCsAndAnOldDaemonSupportsNothingNew(t *testing.T) {
	ctx := context.Background()
	c := newHarness(&fake{}, 1).c
	if ok, err := c.Supports(ctx, "ApplyTransaction"); err != nil || !ok {
		t.Fatalf("ApplyTransaction: %v %v", ok, err)
	}
	if ok, err := c.Supports(ctx, "WatchEvents"); err != nil || ok {
		t.Fatalf("WatchEvents: %v %v", ok, err)
	}
	if ok, err := newHarness(&fake{oldDaemon: true}, 1).c.Supports(ctx, "ApplyTransaction"); err != nil || ok {
		t.Fatalf("old daemon: %v %v", ok, err)
	}
	if _, err := newHarness(&fake{refuse: true}, 1).c.Supports(ctx, "ApplyTransaction"); err == nil {
		t.Fatal("a refusal other than UNIMPLEMENTED was reported as unsupported")
	}
}

func TestWatchEventsResumesAfterTheLastEventAndReportsARestart(t *testing.T) {
	down := status.Error(codes.Unavailable, "down")
	start := func(seq uint64) *pb.Event { return &pb.Event{Type: "bess.start", Sequence: seq, DaemonEpoch: 1} }
	f := &fake{streams: []*scriptedStream{
		{steps: []any{start(5), down}}, // fails before its first event
		{steps: []any{start(5), &pb.Event{Sequence: 5, Type: "a", DaemonEpoch: 1}, &pb.Event{Sequence: 6, Type: "b", DaemonEpoch: 1}, down}},
		{steps: []any{start(7), &pb.Event{Type: "bess.gap", GapFrom: 7, GapTo: 9, DaemonEpoch: 1}, &pb.Event{Sequence: 9, Type: "c", DaemonEpoch: 1},
			&pb.Event{Type: "bess.progress", Sequence: 14, DaemonEpoch: 1}, down}},
		{steps: []any{&pb.Event{Type: "bess.restart", DaemonEpoch: 2}}},
	}}
	h := newHarness(f, 1)
	events, errc := h.c.WatchEvents(context.Background(), 5, 0)
	var seen []string
	for e := range events {
		seen = append(seen, e.GetType())
	}
	var restarted *DaemonRestartedError
	if err := <-errc; !errors.As(err, &restarted) || restarted.To != 2 {
		t.Fatalf("want DaemonRestartedError to epoch 2, got %v", err)
	}
	if strings.Join(seen, ",") != "a,b,bess.gap,c" {
		t.Fatalf("events %v", seen)
	}
	want := [][2]uint64{{5, 0}, {5, 1}, {7, 1}, {15, 1}}
	if fmt.Sprint(f.watched) != fmt.Sprint(want) {
		t.Fatalf("requests %v, want %v (resumed where it stopped, under the epoch seen)", f.watched, want)
	}
}

func TestWatchEventsClosesItsErrorChannelWhenTheContextEnds(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		&pb.Event{Sequence: 1, Type: "a", DaemonEpoch: 1}}}}}
	ctx, cancel := context.WithCancel(context.Background())
	events, errc := newHarness(f, 1).c.WatchEvents(ctx, 0, 0)
	<-events
	cancel()
	for range events {
	}
	select {
	case <-errc: // closed (or an error): the caller's '<-errc' returns
	case <-time.After(5 * time.Second):
		t.Fatal("the error channel stayed open after cancellation")
	}
}

func TestThePipelineBuilderAssemblesAndEditsASnapshot(t *testing.T) {
	wid := int32(0)
	p, err := NewPipeline(nil).Task(&pb.TaskPlacement{Module: "a", Wid: &wid}).
		Task(&pb.TaskPlacement{Module: "b", Parent: "rl"}).Worker(0, 2, "").Module("a", "Source", nil).
		Module("b", "Bypass", &pb.GetTransactionRequest{RequestId: "x"}).Module("c", "Sink", nil).
		Chain("a", "b", "c").Connect("b", 1, "c", 0).Build()
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, c := range p.GetConnections() {
		got = append(got, fmt.Sprintf("%s:%d->%s", c.GetUpstream(), c.GetOgate(), c.GetDownstream()))
	}
	if strings.Join(got, " ") != "a:0->b b:0->c b:1->c" {
		t.Fatal(got)
	}
	if !p.GetModules()[1].GetArg().MessageIs(&pb.GetTransactionRequest{}) {
		t.Fatal(p.GetModules()[1])
	}
	edited, _ := NewPipeline(p).Remove("b").Build()
	if len(edited.GetModules()) != 2 || len(edited.GetConnections()) != 0 || len(p.GetModules()) != 3 {
		t.Fatal(edited, p)
	}
	if len(edited.GetTasks()) != 1 || edited.GetTasks()[0].GetModule() != "a" {
		t.Fatal("the removed module's placement stayed:", edited.GetTasks())
	}
}

// D-123: an automatic worker leaves its core unset (bessd picks the CPU); an
// explicit core, 0 included, is sent.
func TestThePipelineBuilderLeavesAnAutomaticWorkersCoreUnset(t *testing.T) {
	p, err := NewPipeline(nil).AutoWorker(0, "").Worker(1, 0, "").Build()
	if err != nil {
		t.Fatal(err)
	}
	w := p.GetWorkers()
	if len(w) != 2 || w[0].Core != nil || w[1].Core == nil || w[1].GetCore() != 0 {
		t.Fatal(w)
	}
}

func TestApplyPipelineRetriesBusyAndNeverAConflict(t *testing.T) {
	busyStep := refusal(codes.FailedPrecondition, pb.ErrorDetail_RESOURCE_BUSY)
	h := newHarness(&fake{pipelines: []step{busyStep, busyStep}}, 4)
	ctx := context.Background()
	snap, err := h.c.Pipeline(ctx)
	if err != nil {
		t.Fatal(err)
	}
	r, err := h.c.ApplyPipeline(ctx, snap.Pipeline, WithExpectedVersion(snap.Version))
	if err != nil || r.GetGeneration() != 4 || len(h.f.applied) != 3 || h.f.applied[0].GetExpectedGeneration() != 3 ||
		h.f.applied[0].GetDaemonEpoch() != 1 {
		t.Fatal(r, err, h.f.applied)
	}
	if len(h.sleeps) != 2 || h.sleeps[1] != 20*time.Millisecond {
		t.Fatal(h.sleeps)
	}
	h = newHarness(&fake{pipelines: []step{refusal(codes.Aborted, pb.ErrorDetail_CONFLICT)}}, 4)
	_, err = h.c.ApplyPipeline(ctx, &pb.Pipeline{}, WithExpectedVersion(PipelineVersion{Generation: 1, DaemonEpoch: 1}))
	as[*PipelineConflictError](t, err)
	if len(h.f.applied) != 1 {
		t.Fatal(h.f.applied)
	}
	h = newHarness(&fake{pipelines: []step{busyStep, busyStep}}, 2)
	_, err = h.c.ApplyPipeline(ctx, &pb.Pipeline{})
	as[*BusyError](t, err)
}

// No free CPU for an automatic worker (D-123): ResourceExhausted with the
// daemon's detail is an answered refusal -- InvalidRequestError, sent once,
// never retried and never taken for no answer.
func TestApplyPipelineExhaustedIsARefusalNotRetried(t *testing.T) {
	h := newHarness(&fake{pipelines: []step{refusal(codes.ResourceExhausted, pb.ErrorDetail_RESOURCE_FAILURE)}}, 4)
	_, err := h.c.ApplyPipeline(context.Background(), &pb.Pipeline{})
	e := as[*InvalidRequestError](t, err)
	if e.Code != codes.ResourceExhausted || e.Detail.GetCode() != pb.ErrorDetail_RESOURCE_FAILURE {
		t.Fatal(e)
	}
	if len(h.f.applied) != 1 || len(h.sleeps) != 0 {
		t.Fatal(h.f.applied, h.sleeps)
	}
}

// Every apply names an epoch: its expected version's, else the one the client
// learned. A restarted daemon refuses it: Refused, not retried, and the
// client now knows the new epoch.
func TestAnApplyNamesItsEpochAndARestartRefusesIt(t *testing.T) {
	ctx := context.Background()
	f := &fake{}
	h := newHarness(f, 4)
	if _, err := h.c.ApplyPipeline(ctx, &pb.Pipeline{}); err != nil {
		t.Fatal(err)
	}
	if f.listed != 1 || f.applied[0].GetDaemonEpoch() != 1 || f.applied[0].ExpectedGeneration != nil {
		t.Fatal("the epoch was not learned before the apply", f.listed, f.applied[0])
	}
	snap, _ := h.c.Pipeline(ctx)
	f.epoch = 2 // restarted after the snapshot
	_, err := h.c.ApplyPipeline(ctx, snap.Pipeline, WithExpectedVersion(snap.Version))
	if e := as[*DaemonRestartedError](t, err); !e.Refused || e.From != 1 || e.To != 2 {
		t.Fatal(e)
	}
	if epoch, _ := h.c.DaemonEpoch(); len(f.applied) != 2 || epoch != 2 {
		t.Fatal("a refusal was retried, or the new epoch not learned", len(f.applied), epoch)
	}
	// An expected version is sent as given: one without its epoch is the
	// daemon's to refuse, never completed with the current epoch.
	_, err = h.c.ApplyPipeline(ctx, snap.Pipeline, WithExpectedVersion(PipelineVersion{Generation: 3}))
	as[*InvalidRequestError](t, err)
	if last := f.applied[len(f.applied)-1]; last.GetDaemonEpoch() != 0 {
		t.Fatal("an expected version's missing epoch was filled in", last)
	}
}

func TestApplyPipelineWithoutAnAnswerIsUnknownAndNotResent(t *testing.T) {
	h := newHarness(&fake{pipelines: []step{timeout()}}, 4)
	_, err := h.c.ApplyPipeline(context.Background(), &pb.Pipeline{})
	as[*TransportError](t, err)
	if len(h.f.applied) != 1 {
		t.Fatal(h.f.applied)
	}
	// UNAVAILABLE with BESS's detail is an answer (a resource failure).
	h = newHarness(&fake{pipelines: []step{refusal(codes.Unavailable, pb.ErrorDetail_RESOURCE_FAILURE)}}, 4)
	_, err = h.c.ApplyPipeline(context.Background(), &pb.Pipeline{})
	if e := as[*InvalidRequestError](t, err); e.Detail.GetCode() != pb.ErrorDetail_RESOURCE_FAILURE {
		t.Fatal(e)
	}
}

// The Admin service's mutating calls (D-107) follow D-106: sent under the
// epoch the client knows (learned first), refused under another as a
// restart, and not retried.
func TestAdminCallsNameTheKnownEpochAndARestartRefusesThem(t *testing.T) {
	f := &fake{}
	c := newHarness(f, 4).c
	ctx := context.Background()
	if err := c.SetCapture(ctx, &pb.SetCaptureRequest{Module: "m"}); err != nil {
		t.Fatal(err)
	}
	if f.listed != 1 || f.captures[0].GetDaemonEpoch() != 1 {
		t.Fatalf("listed %d, sent under epoch %d: the epoch was not learned first", f.listed, f.captures[0].GetDaemonEpoch())
	}
	f.epoch = 2 // restarted
	err := c.SetCapture(ctx, &pb.SetCaptureRequest{Module: "m"})
	var restarted *DaemonRestartedError
	if !errors.As(err, &restarted) || restarted.From != 1 || restarted.To != 2 || !restarted.Refused {
		t.Fatalf("got %v, want a refused restart from 1 to 2", err)
	}
	if len(f.captures) != 2 {
		t.Fatalf("%d sends: a refusal was retried", len(f.captures))
	}
	if err := c.SetCapture(ctx, &pb.SetCaptureRequest{Module: "m"}); err != nil {
		t.Fatal(err)
	}
	if got := f.captures[2].GetDaemonEpoch(); got != 2 {
		t.Fatalf("next call under epoch %d, want the new epoch 2", got)
	}
}

// Discovery gives each resource its kind (none, from a daemon before D-129:
// a map) and whether it changes only with its workers paused.
func TestResourcesCarryTheirKind(t *testing.T) {
	h := newHarness(&fake{}, 4)
	all, err := h.c.Resources(context.Background(), false)
	if err != nil {
		t.Fatal(err)
	}
	if m := all[resource]; m.Kind != pb.TransactionResource_KIND_MAP || m.RequiresPausedWorkers {
		t.Fatal(m)
	}
	if s := all[singleton]; s.Kind != pb.TransactionResource_KIND_SINGLETON || !s.RequiresPausedWorkers {
		t.Fatal(s)
	}
}

// A singleton is set (an op with a value and no key), never upserted or
// erased, and a map is never set: the client refuses the wrong one before
// anything is sent.
func TestASingletonIsSetAndAMapIsNot(t *testing.T) {
	ctx := context.Background()
	h := newHarness(&fake{applies: []step{answer(applied, 1)}}, 4)
	m, _ := h.c.Resource(ctx, resource)
	s, _ := h.c.Resource(ctx, singleton)
	tx := h.c.Transaction()
	as[*InvalidRequestError](t, tx.Upsert(s, &pb.GetTransactionRequest{}, &pb.ListTransactionResourcesRequest{}))
	as[*InvalidRequestError](t, tx.Erase(s, &pb.GetTransactionRequest{}))
	as[*InvalidRequestError](t, tx.Set(m, &pb.ListTransactionResourcesRequest{}))
	as[*InvalidRequestError](t, tx.Set(s, &pb.GetTransactionRequest{})) // not its value type
	if err := tx.Set(s, &pb.ListTransactionResourcesRequest{}); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if len(h.f.sent) != 1 || len(h.f.sent[0].GetOps()) != 1 {
		t.Fatal("refused operations were sent:", h.f.sent)
	}
	if op := h.f.sent[0].GetOps()[0]; op.GetResource() != singleton || op.GetKey() != nil ||
		!op.GetValue().MessageIs(&pb.ListTransactionResourcesRequest{}) || op.GetErase() {
		t.Fatal(op)
	}
}

// A resource that changes only with its workers paused, refused while one
// runs (RESOURCE_BUSY), is a BusyError at once: sent once, nothing asked,
// not retried (retrying will not help until the workers are paused).
func TestAPausedWorkersRefusalIsBusyAndNotRetried(t *testing.T) {
	h := newHarness(&fake{applies: []step{refusal(codes.FailedPrecondition, pb.ErrorDetail_RESOURCE_BUSY)}}, 4)
	_, err := h.commit(t)
	if e := as[*BusyError](t, err); e.Detail.GetCode() != pb.ErrorDetail_RESOURCE_BUSY || e.Attempts != 1 {
		t.Fatal(e)
	}
	if len(h.f.sent) != 1 || len(h.f.asked) != 0 || len(h.sleeps) != 0 {
		t.Fatalf("sent %d, asked %d, slept %v", len(h.f.sent), len(h.f.asked), h.sleeps)
	}
}

// Pipeline reads resource contents only when asked (WithResources), and only
// then has a resource generation; a diff or plan has one when its pipeline
// lists resources; an apply expects the one its version carries, and a stale
// one is a conflict.
func TestResourceGenerations(t *testing.T) {
	ctx := context.Background()
	f := &fake{}
	h := newHarness(f, 4)
	plain, err := h.c.Pipeline(ctx)
	if err != nil || plain.Version.ResourceGeneration != nil || f.reads[0].GetIncludeResources() {
		t.Fatal(plain.Version, err, f.reads)
	}
	named, err := h.c.Pipeline(ctx, WithResources(resource, singleton))
	if err != nil || named.Version.ResourceGeneration == nil || *named.Version.ResourceGeneration != 9 {
		t.Fatal(named.Version, err)
	}
	if r := f.reads[1]; !r.GetIncludeResources() || !slices.Equal(r.GetResources(), []string{resource, singleton}) {
		t.Fatal(r)
	}
	all, err := h.c.Pipeline(ctx, WithResources())
	if r := f.reads[2]; err != nil || !r.GetIncludeResources() || len(r.GetResources()) != 0 ||
		all.Version.ResourceGeneration == nil {
		t.Fatal(r, all.Version, err)
	}

	listed := &pb.Pipeline{Resources: []*pb.ResourceContents{{Name: resource}}}
	for _, p := range []*pb.Pipeline{{}, listed} {
		_, dv, err := h.c.DiffPipeline(ctx, p)
		if err != nil {
			t.Fatal(err)
		}
		_, pv, err := h.c.PlanPipeline(ctx, p)
		if err != nil {
			t.Fatal(err)
		}
		want := len(p.GetResources()) > 0
		for _, v := range []PipelineVersion{dv, pv} {
			if v.Generation != 3 || (v.ResourceGeneration != nil) != want || (want && *v.ResourceGeneration != 9) {
				t.Fatal(p, v)
			}
		}
	}

	if _, err := h.c.ApplyPipeline(ctx, listed, WithExpectedVersion(named.Version)); err != nil {
		t.Fatal(err)
	}
	if _, err := h.c.ApplyPipeline(ctx, listed, WithExpectedVersion(plain.Version)); err != nil {
		t.Fatal(err)
	}
	if a := f.applied; a[0].ExpectedResourceGeneration == nil || a[0].GetExpectedResourceGeneration() != 9 ||
		a[1].ExpectedResourceGeneration != nil {
		t.Fatal("expected resource generations:", a[0], a[1])
	}
	f.pipelines = []step{refusal(codes.Aborted, pb.ErrorDetail_CONFLICT)}
	_, err = h.c.ApplyPipeline(ctx, listed, WithExpectedVersion(named.Version))
	as[*PipelineConflictError](t, err)
	if len(f.applied) != 3 {
		t.Fatal("a conflict was retried:", len(f.applied))
	}
}

// InvokeAction (D-129) names the module, the action and the epoch the client
// knows (learned first) and returns the action's result. It is not retried:
// a busy module is a BusyError, a refusal (a name the class declares no action
// for: ENOTSUP) an InvalidRequestError with the daemon's detail, and a restart
// refuses it.
func TestInvokeAction(t *testing.T) {
	ctx := context.Background()
	f := &fake{}
	c := newHarness(f, 4).c
	result, err := c.InvokeAction(ctx, "l2", "lookup", &pb.GetTransactionRequest{RequestId: "arg"})
	if err != nil {
		t.Fatal(err)
	}
	got := &pb.GetTransactionRequest{}
	if err := result.UnmarshalTo(got); err != nil || got.GetRequestId() != "answer" {
		t.Fatal(result, err)
	}
	req, arg := f.invoked[0], &pb.GetTransactionRequest{}
	if f.listed != 1 || req.GetDaemonEpoch() != 1 || req.GetModule() != "l2" || req.GetAction() != "lookup" ||
		req.GetArg().UnmarshalTo(arg) != nil || arg.GetRequestId() != "arg" {
		t.Fatal("listed", f.listed, req)
	}

	f.actions = []step{refusal(codes.FailedPrecondition, pb.ErrorDetail_RESOURCE_BUSY)}
	_, err = c.InvokeAction(ctx, "l2", "populate", &pb.GetTransactionRequest{})
	if e := as[*BusyError](t, err); e.Detail.GetCode() != pb.ErrorDetail_RESOURCE_BUSY || len(f.invoked) != 2 {
		t.Fatal(e, len(f.invoked))
	}

	d, _ := proto.Marshal(&pb.ErrorDetail{Code: pb.ErrorDetail_INVALID_ARGUMENT, ErrnoValue: 95, Field: "action",
		Message: "'L2Forward' does not support action 'add'"})
	f.actions = []step{{err: status.Error(codes.InvalidArgument, "x"), md: metadata.Pairs(errorDetailKey, string(d))}}
	_, err = c.InvokeAction(ctx, "l2", "add", &pb.GetTransactionRequest{})
	if e := as[*InvalidRequestError](t, err); e.Detail.GetField() != "action" || e.Detail.GetErrnoValue() != 95 ||
		len(f.invoked) != 3 {
		t.Fatal(e, len(f.invoked))
	}

	f.epoch = 2 // restarted
	_, err = c.InvokeAction(ctx, "l2", "lookup", &pb.GetTransactionRequest{})
	if e := as[*DaemonRestartedError](t, err); !e.Refused || e.From != 1 || e.To != 2 || len(f.invoked) != 4 {
		t.Fatal(e, len(f.invoked))
	}
	if epoch, _ := c.DaemonEpoch(); epoch != 2 {
		t.Fatal("the new epoch was not learned:", epoch)
	}
}
