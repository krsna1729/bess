// SPDX-License-Identifier: BSD-3-Clause
// Counter watches (D-150 to D-152): the crossings a module reports, typed.

package bess

import (
	"context"
	"fmt"
	"math"
	"strconv"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// CounterThresholdType is the event type every counter crossing is reported
// as, by both producers: a module's tick sweeping the totals it keeps, and a
// worker's own count on the packet path.
const CounterThresholdType = "bess.counter_threshold"

// A CounterCrossing is one counter reaching a limit a controller armed.
//
// Who reports it decides what Value means. A module's tick reads every
// worker's plane, so its Value is the key's total and Worker is NoWorker. A
// worker reports its own plane: Worker names it and Value is that worker's
// share, which can be below the total the controller armed on -- a hint to
// read the total, not the answer. A module only reports from a worker when a
// key's traffic runs on one worker.
type CounterCrossing struct {
	Source   string // the module that reported it
	Key      uint64 // the counter's key
	Value    uint64 // what the producer read
	Limit    uint64 // the limit it was armed at
	Worker   int    // the plane that crossed; NoWorker for a tick's report
	Sequence uint64 // the event's sequence in the daemon's log
	TimeNs   uint64 // the daemon's monotonic clock, ns
}

// NoWorker is the Worker of a crossing a module's tick reported: it read every
// worker's plane, so no single one crossed.
const NoWorker = -1

// GapError ends a counter watch when the daemon's log dropped events: a
// crossing of the watched module may be among them. The controller re-reads
// the module's counters and watches again.
type GapError struct {
	Module   string
	From, To uint64 // the sequences the log no longer holds
}

func (e *GapError) Error() string {
	return fmt.Sprintf(
		"bess: the event log dropped events %d to %d, so a crossing of %q may be missing: re-read its counters",
		e.From, e.To, e.Module)
}

// WatchCounters streams the counter crossings `module` reports. It is
// WatchEvents narrowed to CounterThresholdType and to one source, with the
// event's text fields parsed into numbers.
//
// Arming is the module's own business: a module that watches counters keeps a
// quota set as one of its resources (the sample plugin's usage report keeps
// "<name>/quotas"), so a controller arms it by setting that resource in a
// transaction, and reads it back the same way. Discovery names it: Resources
// lists what the module has. Nothing about the watch is in the framework --
// the daemon only carries the events -- so this helper parses the shape the
// producers emit (key, value, limit, and worker when a worker reported it).
//
// Three things end the watch besides its context, each with an error, because
// a controller that misses a crossing has missed a quota:
//
//   - a crossing whose fields do not parse: the event type is a name any
//     producer may register, so a silent skip could hide the watched module's
//     own shape changing;
//   - a gap in the log (*GapError): events the daemon no longer holds, which
//     may include one of this module's crossings, so the controller re-reads
//     the counters it armed;
//   - a daemon restart (*DaemonRestartedError, from WatchEvents): everything
//     built from earlier events is void, so re-read and watch again.
//
// The daemon's other control events never reach this helper: the request's
// type filter (WatchEvents) drops them. A worker's ring refusing an event is
// reported as "bess.events_lost", which names the worker rather than a module,
// so a worker-ring loss can drop this module's own crossings without this
// watch saying so -- a controller that must not miss one watches the raw
// stream instead. The watch starts live: a crossing reported before it started
// is not delivered.
func (c *Client) WatchCounters(ctx context.Context, module string) (<-chan CounterCrossing, <-chan error) {
	ctx, cancel := context.WithCancel(ctx)
	events, inner := c.WatchEvents(ctx, 0, 0, CounterThresholdType)
	out := make(chan CounterCrossing)
	errc := make(chan error, 1)
	go func() {
		defer cancel() // releases the inner stream when this one ends
		defer close(errc)
		defer close(out)
		for e := range events {
			switch e.GetType() {
			case CounterThresholdType:
			case "bess.gap":
				errc <- &GapError{Module: module, From: e.GetGapFrom(), To: e.GetGapTo()}
				return
			default:
				continue // a control event that is not about this module's keys
			}
			if e.GetSource() != module {
				continue
			}
			crossing, err := parseCrossing(e)
			if err != nil {
				errc <- err
				return
			}
			select {
			case out <- crossing:
			case <-ctx.Done():
				return
			}
		}
		// WatchEvents closes events before it closes its error channel, so a
		// receive here is the stream's error, if it ended with one. That
		// ordering is what makes this safe, and it rests on the inner error
		// channel being buffered (client.go): an unbuffered one would block
		// its goroutine's send before it could close events, and this receive
		// would never run.
		if err, ok := <-inner; ok {
			errc <- err
		}
	}()
	return out, errc
}

func parseCrossing(e *pb.Event) (CounterCrossing, error) {
	fields := e.GetFields()
	key, err := crossingField(fields, e.GetSource(), "key")
	if err != nil {
		return CounterCrossing{}, err
	}
	value, err := crossingField(fields, e.GetSource(), "value")
	if err != nil {
		return CounterCrossing{}, err
	}
	limit, err := crossingField(fields, e.GetSource(), "limit")
	if err != nil {
		return CounterCrossing{}, err
	}
	worker := NoWorker
	if text, ok := fields["worker"]; ok {
		parsed, err := strconv.ParseUint(text, 10, 32)
		if err != nil {
			return CounterCrossing{}, fmt.Errorf("bess: crossing from %q has worker %q: %w",
				e.GetSource(), text, err)
		}
		// CounterCrossing.Worker is an int, so a value that does not fit one
		// (a worker id is small, but the field is a uint32 on the wire) is
		// refused rather than narrowed.
		if parsed > math.MaxInt32 {
			return CounterCrossing{}, fmt.Errorf("bess: crossing from %q has worker %q, above %d",
				e.GetSource(), text, math.MaxInt32)
		}
		worker = int(parsed)
	}
	return CounterCrossing{
		Source:   e.GetSource(),
		Key:      key,
		Value:    value,
		Limit:    limit,
		Worker:   worker,
		Sequence: e.GetSequence(),
		TimeNs:   e.GetTimeNs(),
	}, nil
}

func crossingField(fields map[string]string, source, name string) (uint64, error) {
	text, ok := fields[name]
	if !ok {
		return 0, fmt.Errorf("bess: crossing from %q has no %q field (fields: %v)",
			source, name, fields)
	}
	parsed, err := strconv.ParseUint(text, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("bess: crossing from %q has %s %q: %w", source, name, text, err)
	}
	return parsed, nil
}
