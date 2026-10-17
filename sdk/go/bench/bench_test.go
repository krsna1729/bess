// SPDX-License-Identifier: BSD-3-Clause

// These need no daemon, so CI exercises what this package promises a caller:
// the fit's two terms, the normalisation against them, and Measure's median.
package bench

import (
	"context"
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

func abs(d time.Duration) time.Duration {
	if d < 0 {
		return -d
	}
	return d
}
