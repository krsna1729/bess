// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"context"
	"errors"
	"strings"
	"testing"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

// crossing builds the event both producers emit: key, value and limit always,
// worker only when a worker's own plane crossed.
func crossing(source string, seq uint64, fields map[string]string) *pb.Event {
	return &pb.Event{Sequence: seq, Type: CounterThresholdType, Source: source,
		DaemonEpoch: 1, TimeNs: seq * 10, Fields: fields}
}

func TestWatchCountersDeliversOneModulesCrossingsTyped(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		crossing("other", 1, map[string]string{"key": "1", "value": "2", "limit": "3"}),
		crossing("usage", 2, map[string]string{"key": "7", "value": "100", "limit": "100"}),
		crossing("usage", 3, map[string]string{"key": "9", "value": "50", "limit": "50", "worker": "3"}),
		&pb.Event{Type: "bess.restart", DaemonEpoch: 2},
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")

	var seen []CounterCrossing
	for c := range out {
		seen = append(seen, c)
	}
	if len(seen) != 2 {
		t.Fatalf("delivered %d crossings, want 2 (only the named module's): %+v", len(seen), seen)
	}
	tick := seen[0]
	if tick.Source != "usage" || tick.Key != 7 || tick.Value != 100 || tick.Limit != 100 {
		t.Fatalf("the tick's crossing: %+v", tick)
	}
	if tick.Worker != NoWorker {
		t.Fatalf("a tick reads every plane, so its Worker is NoWorker, got %d", tick.Worker)
	}
	if tick.Sequence != 2 || tick.TimeNs != 20 {
		t.Fatalf("sequence and time: %+v", tick)
	}
	if worker := seen[1]; worker.Key != 9 || worker.Worker != 3 || worker.Value != 50 {
		t.Fatalf("the worker's crossing: %+v", worker)
	}
	var restarted *DaemonRestartedError
	if err := <-errc; !errors.As(err, &restarted) || restarted.To != 2 {
		t.Fatalf("the stream's own error was not forwarded: %v", err)
	}
	if len(f.watchedTypes) == 0 {
		t.Fatal("no watch was made")
	}
	for _, types := range f.watchedTypes {
		if strings.Join(types, ",") != CounterThresholdType {
			t.Fatalf("asked for %v, want only %s", types, CounterThresholdType)
		}
	}
}

func TestWatchCountersStopsOnACrossingItCannotParse(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		crossing("usage", 2, map[string]string{"key": "7", "limit": "100"}),
		status.Error(codes.Unavailable, "down"),
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")

	for range out {
	}
	err := <-errc
	if err == nil || !strings.Contains(err.Error(), `"value"`) {
		t.Fatalf("want an error naming the missing field, got %v", err)
	}
}

func TestWatchCountersStopsOnAValueThatIsNotANumber(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		crossing("usage", 2, map[string]string{"key": "7", "value": "1_000", "limit": "100"}),
		status.Error(codes.Unavailable, "down"),
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")

	for range out {
	}
	err := <-errc
	if err == nil || !strings.Contains(err.Error(), `"1_000"`) {
		t.Fatalf("want an error naming the value it could not parse, got %v", err)
	}
}

func TestWatchCountersStopsOnAGapInTheLog(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		crossing("usage", 2, map[string]string{"key": "7", "value": "100", "limit": "100"}),
		&pb.Event{Type: "bess.gap", GapFrom: 3, GapTo: 9, DaemonEpoch: 1},
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")

	seen := 0
	for range out {
		seen++
	}
	if seen != 1 {
		t.Fatalf("delivered %d crossings before the gap, want 1", seen)
	}
	var gap *GapError
	err := <-errc
	if !errors.As(err, &gap) {
		t.Fatalf("want a GapError, got %v", err)
	}
	if gap.Module != "usage" || gap.From != 3 || gap.To != 9 {
		t.Fatalf("the gap's range: %+v", gap)
	}
}

func TestWatchCountersStopsOnAWorkerAboveWhatAnIntHolds(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		crossing("usage", 2, map[string]string{"key": "7", "value": "100", "limit": "100",
			"worker": "4294967295"}),
		status.Error(codes.Unavailable, "down"),
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")

	for range out {
	}
	err := <-errc
	if err == nil || !strings.Contains(err.Error(), "4294967295") {
		t.Fatalf("want an error naming the worker it could not narrow, got %v", err)
	}
}

func TestWatchCountersDrawsItsBoundsWhereGoDoes(t *testing.T) {
	// A worker is bounded by what an int holds on every platform (MaxInt32),
	// and key/value/limit by ParseUint's 64 bits. Both are tested at the edge:
	// the largest accepted worker, and one above each bound.
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		crossing("usage", 2, map[string]string{"key": "7", "value": "100", "limit": "100",
			"worker": "2147483647"}),
		&pb.Event{Type: "bess.restart", DaemonEpoch: 2},
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")
	var seen []CounterCrossing
	for c := range out {
		seen = append(seen, c)
	}
	if len(seen) != 1 || seen[0].Worker != 2147483647 {
		t.Fatalf("the largest worker an int holds is accepted: %+v", seen)
	}
	for range errc {
	}

	for _, tc := range []struct{ name, field, value string }{
		{"a worker above an int", "worker", "2147483648"},
		{"a key above 64 bits", "key", "18446744073709551616"},
		{"a value above 64 bits", "value", "18446744073709551616"},
		{"a limit above 64 bits", "limit", "18446744073709551616"},
	} {
		fields := map[string]string{"key": "7", "value": "100", "limit": "100"}
		fields[tc.field] = tc.value
		f := &fake{streams: []*scriptedStream{{steps: []any{
			&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
			crossing("usage", 2, fields),
			status.Error(codes.Unavailable, "down"),
		}}}}
		out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")
		for range out {
		}
		if err := <-errc; err == nil || !strings.Contains(err.Error(), tc.value) {
			t.Fatalf("%s: want an error naming %q, got %v", tc.name, tc.value, err)
		}
	}
}

func TestWatchCountersPassesOverAnotherModulesLostEvents(t *testing.T) {
	f := &fake{streams: []*scriptedStream{{steps: []any{
		&pb.Event{Type: "bess.start", Sequence: 1, DaemonEpoch: 1},
		&pb.Event{Type: "bess.events_lost", Sequence: 2, Source: "worker1", DaemonEpoch: 1},
		crossing("usage", 3, map[string]string{"key": "7", "value": "100", "limit": "100"}),
		&pb.Event{Type: "bess.restart", DaemonEpoch: 2},
	}}}}
	out, errc := newHarness(f, 1).c.WatchCounters(context.Background(), "usage")

	seen := 0
	for range out {
		seen++
	}
	if seen != 1 {
		t.Fatalf("delivered %d crossings, want 1: a worker's lost events name a worker, not a module", seen)
	}
	var restarted *DaemonRestartedError
	if err := <-errc; !errors.As(err, &restarted) {
		t.Fatalf("a restart still ends the watch: %v", err)
	}
}
