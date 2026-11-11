// SPDX-License-Identifier: BSD-3-Clause
package bench

import (
	"context"
	"encoding/binary"
	"errors"
	"sync"
	"testing"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/anypb"
)

func TestStageSelectionChangesEachRevisitedKey(t *testing.T) {
	for _, locality := range []string{"hot", "spread"} {
		t.Run(locality, func(t *testing.T) {
			const resident = 1024
			last := make([]uint32, resident)
			for i := range last {
				last[i] = 1
			}
			// Ordinal zero is the cold write. Continue its state into the
			// measured ordinals, including multiple full spread passes.
			for ordinal := range 3 * resident {
				slot, gate := stageSelection(ordinal, resident, locality)
				if slot < 0 || slot >= resident || gate < 1 || gate > 2 {
					t.Fatalf("ordinal %d selected slot %d gate %d", ordinal, slot, gate)
				}
				if gate == last[slot] {
					t.Fatalf("ordinal %d leaves slot %d at gate %d", ordinal, slot, gate)
				}
				if locality == "hot" && slot != 0 {
					t.Fatalf("hot fixture selected slot %d", slot)
				}
				last[slot] = gate
			}
		})
	}
}

func TestStageIterationBoundaries(t *testing.T) {
	cfg := StageConfig{Counts: []int{1, 2, 3, 4, 8}, Resident: 1024, Mode: "message", Locality: "hot"}
	for _, iterations := range []int{1, 65536} {
		cfg.Iterations = iterations
		if err := validateStageConfig(cfg); err != nil {
			t.Fatalf("%d iterations rejected: %v", iterations, err)
		}
	}
	for _, iterations := range []int{-1, 0, 65537, int(^uint(0) >> 1)} {
		cfg.Iterations = iterations
		// A nil client proves rejection happens before any graph/RPC work.
		if _, err := RunStages(context.Background(), nil, cfg); err == nil {
			t.Fatalf("%d iterations accepted", iterations)
		}
	}
}

func TestStageResidentFixtureValidation(t *testing.T) {
	const resident = 1024
	expected := make(map[string]struct{}, resident)
	values := [][]byte{nil, {0x08, 1}, {0x08, 2}}
	fixture := &pb.ResourceContents{Name: "stage0/rules"}
	for slot := range resident {
		// Opaque encoded keys must match byte-for-byte, not just by count.
		key := make([]byte, 4)
		binary.BigEndian.PutUint32(key, 0x0b000001+uint32(slot))
		expected[string(key)] = struct{}{}
		fixture.Entries = append(fixture.Entries, &pb.ResourceEntry{
			Key:   &anypb.Any{TypeUrl: "type.googleapis.com/bess.pb.ExactMatchRuleKey", Value: key},
			Value: &anypb.Any{TypeUrl: "type.googleapis.com/bess.pb.ExactMatchRuleValue", Value: values[1+slot%2]},
		})
	}
	tests := []struct {
		name   string
		change func(*pb.ResourceContents)
		count  int
		bad    bool
	}{
		{name: "empty", change: func(c *pb.ResourceContents) { c.Entries = nil }},
		{name: "exact fixture with either gate", count: resident},
		{name: "partial fixture", change: func(c *pb.ResourceContents) { c.Entries = c.Entries[:resident-1] }, bad: true},
		{name: "extra caller entry", change: func(c *pb.ResourceContents) {
			c.Entries = append(c.Entries, proto.Clone(c.Entries[0]).(*pb.ResourceEntry))
		}, bad: true},
		{name: "same count wrong byte key", change: func(c *pb.ResourceContents) { c.Entries[0].Key.Value[0] ^= 1 }, bad: true},
		{name: "duplicate key", change: func(c *pb.ResourceContents) { c.Entries[0].Key = c.Entries[1].Key }, bad: true},
		{name: "wrong value", change: func(c *pb.ResourceContents) { c.Entries[0].Value.Value = []byte{0x08, 3} }, bad: true},
		{name: "wrong type", change: func(c *pb.ResourceContents) { c.Entries[0].Key.TypeUrl = "type.googleapis.com/other.Key" }, bad: true},
		{name: "missing key", change: func(c *pb.ResourceContents) { c.Entries[0].Key = nil }, bad: true},
		{name: "missing value", change: func(c *pb.ResourceContents) { c.Entries[0].Value = nil }, bad: true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			contents := proto.Clone(fixture).(*pb.ResourceContents)
			if tt.change != nil {
				tt.change(contents)
			}
			before := proto.Clone(contents)
			count, err := stageResidentCount(contents, expected, values)
			if (err != nil) != tt.bad || (!tt.bad && count != tt.count) {
				t.Fatalf("count=%d error=%v; want count=%d bad=%v", count, err, tt.count, tt.bad)
			}
			if !proto.Equal(contents, before) {
				t.Fatal("validation mutated caller-owned entries")
			}
		})
	}
}

func TestStageConcurrencyRejectsOverlappingOrUnboundedLanes(t *testing.T) {
	cfg := StageConfig{Counts: []int{1, 8}, Iterations: 64, Resident: 1024, Mode: "encoded", Locality: "spread"}
	for _, concurrency := range []int{1, 2, 4, 8, 64} {
		cfg.Concurrency = concurrency
		if err := validateStageConfig(cfg); err != nil {
			t.Fatalf("concurrency %d rejected: %v", concurrency, err)
		}
	}
	for _, concurrency := range []int{-1, 3, 65, 128} {
		cfg.Concurrency = concurrency
		if _, err := RunStages(context.Background(), nil, cfg); err == nil {
			t.Fatalf("concurrency %d accepted", concurrency)
		}
	}
	cfg.Concurrency = 4
	for _, bad := range []StageConfig{
		func() StageConfig { c := cfg; c.Locality = "hot"; return c }(),
		func() StageConfig { c := cfg; c.Resident = 63; return c }(),
		func() StageConfig { c := cfg; c.Iterations = 3; return c }(),
	} {
		if _, err := RunStages(context.Background(), nil, bad); err == nil {
			t.Fatalf("overlapping lanes or empty caller accepted: %+v", bad)
		}
	}
}

func TestStageWindowBoundsCallersAndOrdersKeyRevisits(t *testing.T) {
	const concurrency, resident, iterations = 4, 8, 32
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	started, release := make(chan struct{}, concurrency), make(chan struct{})
	var mu sync.Mutex
	active, peak := 0, 0
	gates := [resident]uint32{2, 1, 1, 1, 1, 1, 1, 1}
	visits := [resident]int{}
	workers := make([]StageWorkerOutcome, concurrency)
	type result struct {
		accepted int
		err      error
	}
	done := make(chan result, 1)
	go func() {
		accepted, err := stageWindow(ctx, iterations, workers,
			func(ctx context.Context, ordinal int) (StageOutcome, error) {
				mu.Lock()
				active++
				peak = max(peak, active)
				mu.Unlock()
				defer func() { mu.Lock(); active--; mu.Unlock() }()
				if ordinal <= concurrency {
					started <- struct{}{}
					select {
					case <-release:
					case <-ctx.Done():
						return StageOutcome{Ordinal: ordinal}, ctx.Err()
					}
				}
				slot, gate := stageSelection(ordinal, resident, "spread")
				mu.Lock()
				if gates[slot] == gate {
					t.Errorf("key %d revisited without changing gate at ordinal %d", slot, ordinal)
				}
				gates[slot] = gate
				visits[slot]++
				mu.Unlock()
				return StageOutcome{Ordinal: ordinal}, nil
			})
		done <- result{accepted, err}
	}()
	for range concurrency {
		select {
		case <-started:
		case <-ctx.Done():
			t.Fatal("bounded callers did not all enter")
		}
	}
	close(release)
	got := <-done
	if got.err != nil || got.accepted != iterations {
		t.Fatalf("accepted %d: %v", got.accepted, got.err)
	}
	if peak != concurrency {
		t.Fatalf("maximum outstanding calls %d, want %d", peak, concurrency)
	}
	for slot, n := range visits {
		if n != iterations/resident {
			t.Errorf("key %d visited %d times", slot, n)
		}
	}
}

func TestStageWindowFailureCancelsPeersWithoutCountingFailedCalls(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	refused := errors.New("refused")
	workers := make([]StageWorkerOutcome, 2)
	started, release := make(chan struct{}, 2), make(chan struct{})
	type result struct {
		accepted int
		err      error
	}
	done := make(chan result, 1)
	go func() {
		accepted, err := stageWindow(ctx, 32, workers,
			func(ctx context.Context, ordinal int) (StageOutcome, error) {
				started <- struct{}{}
				select {
				case <-release:
				case <-ctx.Done():
					return StageOutcome{Ordinal: ordinal}, ctx.Err()
				}
				if ordinal == 1 {
					return StageOutcome{Ordinal: ordinal}, refused
				}
				<-ctx.Done()
				return StageOutcome{Ordinal: ordinal}, ctx.Err()
			})
		done <- result{accepted, err}
	}()
	for range 2 {
		select {
		case <-started:
		case <-ctx.Done():
			t.Fatal("peers did not enter")
		}
	}
	close(release)
	got := <-done
	if !errors.Is(got.err, refused) || got.accepted != 0 {
		t.Fatalf("failed calls counted as accepted: %d, %v", got.accepted, got.err)
	}
	if ctx.Err() != nil {
		t.Fatal("window canceled its caller's context")
	}
}
