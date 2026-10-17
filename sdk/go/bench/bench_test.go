// SPDX-License-Identifier: BSD-3-Clause

// These need no daemon, so CI exercises what this package promises a caller:
// the fit's two terms, the normalisation against them, and Measure's median.
package bench

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestFitRecoversALine: shapes taken from a known line give that line back, so
// a caller's numbers and this package's agree on what the two terms mean.
func TestFitRecoversALine(t *testing.T) {
	shapes := []Shape{
		{Ops: 2, P50: 98 * time.Microsecond},
		{Ops: 8, P50: 122 * time.Microsecond},
		{Ops: 16, P50: 154 * time.Microsecond},
		{Ops: 32, P50: 218 * time.Microsecond},
	}
	floor, err := Fit(shapes)
	if err != nil {
		t.Fatalf("fit: %v", err)
	}
	if d := abs(floor.Fixed - 90*time.Microsecond); d > time.Microsecond {
		t.Errorf("fixed: got %v, want about 90us", floor.Fixed)
	}
	if d := abs(floor.PerOp - 4*time.Microsecond); d > 100*time.Nanosecond {
		t.Errorf("per op: got %v, want about 4us", floor.PerOp)
	}
}

// TestFitRejectsDegenerateInput: a line needs two shapes, spread between them,
// and a slope that is not noise.
func TestFitRejectsDegenerateInput(t *testing.T) {
	if _, err := Fit([]Shape{{Ops: 2, P50: time.Microsecond}}); err == nil {
		t.Error("one shape cannot give a line")
	}
	if _, err := Fit([]Shape{{Ops: 8, P50: time.Microsecond}, {Ops: 8, P50: 2 * time.Microsecond}}); err == nil {
		t.Error("no spread in operation count cannot give a line")
	}
	if _, err := Fit([]Shape{{Ops: 2, P50: 200 * time.Microsecond}, {Ops: 16, P50: 100 * time.Microsecond}}); err == nil {
		t.Error("faster with more operations is noise, not a line")
	}
}

// TestCompareSubtractsTheFloorFixedCost: a sample's per-operation cost is what
// it spends beyond the floor's fixed part, and the multiple is against the
// floor's per-operation cost, which is the normalisation the floor exists for.
func TestCompareSubtractsTheFloorFixedCost(t *testing.T) {
	floor := Floor{Fixed: 90 * time.Microsecond, PerOp: 4 * time.Microsecond}
	c := Compare(floor, Sample{Ops: 11, Latency: 2100 * time.Microsecond, Label: "establish"})
	if len(c.Excess) != 1 {
		t.Fatalf("excess: %v", c.Excess)
	}
	want := (2100*time.Microsecond - 90*time.Microsecond) / 11
	if d := abs(c.Excess[0].PerOp - want); d > time.Microsecond {
		t.Errorf("per op: got %v, want %v", c.Excess[0].PerOp, want)
	}
	if c.Excess[0].Multiple < 40 || c.Excess[0].Multiple > 50 {
		t.Errorf("multiple: got %.1f, want about 45", c.Excess[0].Multiple)
	}
	if c.HasFit {
		t.Error("one sample cannot give the caller their own line")
	}
}

// TestCompareFitsTheCaller: with two shapes the caller's own slope is compared
// with the floor's, which is the comparison that does not assume the floor's
// fixed cost applies to another stack.
func TestCompareFitsTheCaller(t *testing.T) {
	floor := Floor{Fixed: 90 * time.Microsecond, PerOp: 4 * time.Microsecond}
	c := Compare(floor,
		Sample{Ops: 4, Latency: 410 * time.Microsecond},
		Sample{Ops: 11, Latency: 1000 * time.Microsecond})
	if !c.HasFit {
		t.Fatalf("two shapes should give the caller a line: %+v", c)
	}
	if !c.PerOpHigh {
		t.Error("a line above the floor should say so")
	}
	want := (1000*time.Microsecond - 410*time.Microsecond) / 7
	if d := abs(c.OwnPerOp - want); d > time.Microsecond {
		t.Errorf("own per op: got %v, want %v", c.OwnPerOp, want)
	}
	if d := abs(c.OwnFixed - 72*time.Microsecond); d > 2*time.Microsecond {
		t.Errorf("own fixed: got %v, want about 73us", c.OwnFixed)
	}
}

// TestMeasureTakesTheMedian: Measure is the programmatic way in, so it reports
// one number per shape, the median, and not a mean a single slow call moves.
func TestMeasureTakesTheMedian(t *testing.T) {
	calls := 0
	s, err := Measure(context.Background(), 11, 5, "mine", func(context.Context) error {
		calls++
		time.Sleep(time.Duration(calls) * 100 * time.Microsecond)
		return nil
	})
	if err != nil {
		t.Fatalf("measure: %v", err)
	}
	if calls != 5 {
		t.Errorf("calls: %d, want 5", calls)
	}
	if s.Ops != 11 || s.Samples != 5 || s.Label != "mine" {
		t.Errorf("sample: %+v", s)
	}
	if s.Latency < 300*time.Microsecond {
		t.Errorf("the median of 100..500us sleeps is at least 300us, got %v", s.Latency)
	}
}

// TestCompareRejectsTwoDifferentPaths: samples that are not one operation at two
// sizes fit a fixed part at or below zero, which is not a line. The per-sample
// excesses are all that can be read, and the comparison has to say so instead of
// printing a negative fixed cost.
func TestCompareRejectsTwoDifferentPaths(t *testing.T) {
	floor := Floor{Fixed: 90 * time.Microsecond, PerOp: 4 * time.Microsecond}
	c := Compare(floor,
		Sample{Ops: 4, Latency: 410 * time.Microsecond, Label: "modify"},
		Sample{Ops: 11, Latency: 2100 * time.Microsecond, Label: "establish"})
	if c.HasFit {
		t.Errorf("two different operations should not give a line: fixed %v", c.OwnFixed)
	}
	if !c.OwnLineRejected {
		t.Error("the comparison should say why the caller has no line")
	}
	if len(c.Excess) != 2 {
		t.Errorf("the per-sample excesses are still the answer: %v", c.Excess)
	}
	if !strings.Contains(c.String(), "one operation at two sizes") {
		t.Errorf("the report should explain itself:\n%s", c.String())
	}
}

// TestJoinCleanupKeepsBothErrors: a cleanup that fails leaves the daemon in a
// state the caller has to hear about, whether or not the run also failed. The
// error a caller sees must carry both, and a cleanup that succeeded must add
// nothing at all -- an empty wrapper would read as a failure.
func TestJoinCleanupKeepsBothErrors(t *testing.T) {
	runErr := errors.New("the churn was refused")
	cleanupErr := errors.New("no route to the daemon")

	if got := joinCleanup(nil, runErr); got != runErr {
		t.Errorf("a cleanup that succeeded must not wrap the run's error: %v", got)
	}
	if got := joinCleanup(nil, nil); got != nil {
		t.Errorf("neither failing is not an error: %v", got)
	}

	onlyCleanup := joinCleanup(cleanupErr, nil)
	if !errors.Is(onlyCleanup, cleanupErr) {
		t.Errorf("a cleanup failure on its own is the run's error: %v", onlyCleanup)
	}
	if !strings.Contains(onlyCleanup.Error(), "removing the bench graph") {
		t.Errorf("the message should say what failed: %v", onlyCleanup)
	}

	both := joinCleanup(cleanupErr, runErr)
	if !errors.Is(both, runErr) || !errors.Is(both, cleanupErr) {
		t.Errorf("both failures belong in the error: %v", both)
	}
}

// stub is a pause or a resume that does nothing but report err.
func stub(err error) func(context.Context) error {
	return func(context.Context) error { return err }
}

// TestPausedReportsEveryFailure: the pause is where a daemon can be left paused,
// so neither half may vanish. A failed pause must not stop the body, because the
// body is what puts the caller's own pipeline back; a failed resume must reach
// the caller, because nothing else would say the daemon is paused.
func TestPausedReportsEveryFailure(t *testing.T) {
	ctx := context.Background()
	bodyErr := errors.New("the graph would not come off")
	pauseErr := errors.New("no route to the daemon")
	resumeErr := errors.New("the workers would not start")

	ran := false
	if got := paused(ctx, stub(nil), stub(nil), func() error { ran = true; return nil }); got != nil || !ran {
		t.Errorf("a clean pause runs the body and adds nothing: %v (ran=%v)", got, ran)
	}

	ran = false
	got := paused(ctx, stub(pauseErr), stub(nil), func() error { ran = true; return nil })
	if !ran || !errors.Is(got, pauseErr) || !strings.Contains(got.Error(), "pausing the workers") {
		t.Errorf("a failed pause runs the body and is reported: %v (ran=%v)", got, ran)
	}

	got = paused(ctx, stub(nil), stub(resumeErr), func() error { return nil })
	if !errors.Is(got, resumeErr) || !strings.Contains(got.Error(), "resuming the workers") {
		t.Errorf("a failed resume is the caller's only warning: %v", got)
	}

	got = paused(ctx, stub(pauseErr), stub(resumeErr), func() error { return bodyErr })
	if !errors.Is(got, bodyErr) || !errors.Is(got, pauseErr) || !errors.Is(got, resumeErr) {
		t.Errorf("every failure belongs in the error: %v", got)
	}
}

func abs(d time.Duration) time.Duration {
	if d < 0 {
		return -d
	}
	return d
}
