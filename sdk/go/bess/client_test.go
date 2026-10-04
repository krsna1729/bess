// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

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
}

func (f *fake) pipeline(_ context.Context, in proto.Message) (proto.Message, metadata.MD, error) {
	switch req := in.(type) {
	case *pb.GetPipelineRequest:
		return &pb.GetPipelineResponse{Pipeline: &pb.Pipeline{}, Generation: 3}, nil, nil
	case *pb.ApplyPipelineRequest:
		f.applied = append(f.applied, req)
		if len(f.pipelines) > 0 {
			s := f.pipelines[0]
			f.pipelines = f.pipelines[1:]
			if s.err != nil {
				return nil, s.md, s.err
			}
		}
		return &pb.ApplyPipelineResponse{Generation: 4}, nil, nil
	}
	return nil, nil, status.Error(codes.Unimplemented, "fake")
}

func refusal(code codes.Code, detail pb.ErrorDetail_Code) step {
	d, _ := proto.Marshal(&pb.ErrorDetail{Code: detail, Message: "refused"})
	return step{err: status.Error(code, "x"), md: metadata.Pairs(errorDetailKey, string(d))}
}

func (f *fake) apply(_ context.Context, in *pb.ApplyTransactionRequest) (*pb.ApplyTransactionResponse, metadata.MD, error) {
	f.sent = append(f.sent, proto.Clone(in).(*pb.ApplyTransactionRequest))
	s := f.applies[0]
	f.applies = f.applies[1:]
	if s.err != nil {
		return nil, s.md, s.err
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
	return &pb.ListTransactionResourcesResponse{DaemonEpoch: epoch, Resources: []*pb.TransactionResource{
		{Name: resource, KeyType: keyType, ValueType: valueType}}}, nil, nil
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
	r := Resource{resource, keyType, valueType, 1} // no discovery through this client
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
	as[*DaemonRestartedError](t, err)
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
	p, err := NewPipeline(nil).Worker(0, 2, "").Module("a", "Source", nil).
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
}

func TestApplyPipelineRetriesBusyAndNeverAConflict(t *testing.T) {
	busyStep := refusal(codes.FailedPrecondition, pb.ErrorDetail_RESOURCE_BUSY)
	h := newHarness(&fake{pipelines: []step{busyStep, busyStep}}, 4)
	ctx := context.Background()
	snap, err := h.c.Pipeline(ctx)
	if err != nil {
		t.Fatal(err)
	}
	r, err := h.c.ApplyPipeline(ctx, snap.Pipeline, WithPipelineGeneration(snap.Generation))
	if err != nil || r.GetGeneration() != 4 || len(h.f.applied) != 3 || h.f.applied[0].GetExpectedGeneration() != 3 {
		t.Fatal(r, err, h.f.applied)
	}
	if len(h.sleeps) != 2 || h.sleeps[1] != 20*time.Millisecond {
		t.Fatal(h.sleeps)
	}
	h = newHarness(&fake{pipelines: []step{refusal(codes.Aborted, pb.ErrorDetail_CONFLICT)}}, 4)
	_, err = h.c.ApplyPipeline(ctx, &pb.Pipeline{}, WithPipelineGeneration(1))
	as[*PipelineConflictError](t, err)
	if len(h.f.applied) != 1 {
		t.Fatal(h.f.applied)
	}
	h = newHarness(&fake{pipelines: []step{busyStep, busyStep}}, 2)
	_, err = h.c.ApplyPipeline(ctx, &pb.Pipeline{})
	as[*BusyError](t, err)
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
