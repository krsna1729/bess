// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"context"
	"errors"
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
	r := Resource{resource, keyType, valueType} // no discovery through this client
	_ = tx.Upsert(r, &pb.GetTransactionRequest{}, &pb.ListTransactionResourcesRequest{})
	_, err := tx.Commit(context.Background())
	as[*DaemonRestartedError](t, err)
	if h.f.listed != 1 {
		t.Fatal("epoch not learned first")
	}
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
