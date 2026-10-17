// SPDX-License-Identifier: BSD-3-Clause

// Command bessbench measures a running bessd's transaction floor through the Go
// SDK, and places your own control path against it.
//
// The floor is two numbers: a fixed cost per transaction and a cost per
// operation inside it, from a sweep over transaction sizes. Your path can then
// be compared against them without either side's shape matching: count the
// operations one of your transactions sends, time its commit, and pass the pair.
//
//	# this daemon's floor on this machine, and how an 11-operation session
//	# establishment of yours sits against it
//	go run github.com/krsna1729/bess/sdk/go/cmd/bessbench \
//	    -addr localhost:10514 -compare 'establish=11:2.1ms,modify=4:410us'
//
// The command brings its own graph: Source -> RandomUpdate -> ExactMatch ->
// Sinks, on one worker, with the rules it churns. It refuses to touch a daemon
// that already runs a pipeline unless -force is given, it restores whatever was
// there when it finishes or is interrupted, and -no-graph uses an em/rules that
// a pipeline has already. Otherwise it would measure whatever graph happened
// to be loaded, and one run against someone's daemon would wipe it.
//
// The same measurement is available as a package, for your own benchmark code:
// see github.com/krsna1729/bess/sdk/go/bench.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	bench "github.com/krsna1729/bess/sdk/go/bench"
	bess "github.com/krsna1729/bess/sdk/go/bess"
)

func main() {
	log.SetFlags(0)
	if err := run(); err != nil {
		log.Fatalf("bessbench: %v", err)
	}
}

func run() (err error) {
	addr := flag.String("addr", "localhost:10514", "the daemon's control endpoint")
	shapesFlag := flag.String("shapes", "2,8,16,32", "operations per transaction, even and comma separated")
	seconds := flag.Duration("seconds", 2*time.Second, "each shape's measurement window")
	compare := flag.String("compare", "", "your own measurements as label=ops:duration[,...], e.g. 'establish=11:2.1ms'")
	noGraph := flag.Bool("no-graph", false, "use a pipeline that already has em/rules instead of applying the bench graph")
	force := flag.Bool("force", false, "replace the daemon's pipeline for the run, restoring it afterwards")
	workerCore := flag.Int("worker-core", 2, "CPU for the bench graph's worker: not the daemon's control CPU, and not a busy one")
	resource := flag.String("resource", "em/rules", "the map resource to churn")
	preload := flag.Int("preload", 1000, "rules loaded before measuring")
	pktSize := flag.Int("pkt-size", 60, "the bench graph's packet size")
	gates := flag.Int("gates", 4, "gates the rules steer to")
	baseFlag := flag.String("address-base", "0x0b000000", "first key address")
	asJSON := flag.Bool("json", false, "print the sweep, the floor and the comparison as JSON")
	timeout := flag.Duration("timeout", 5*time.Minute, "how long the whole run may take")
	flag.Parse()

	shapes, err := shapeList(*shapesFlag)
	if err != nil {
		return err
	}
	base, err := strconv.ParseUint(strings.TrimPrefix(*baseFlag, "0x"), 16, 32)
	if err != nil {
		return fmt.Errorf("-address-base %q: %w", *baseFlag, err)
	}
	samples, err := sampleList(*compare)
	if err != nil {
		return err
	}

	// Interrupt safe: the bench graph comes off the daemon even on Ctrl-C.
	signals, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	ctx, cancel := context.WithTimeout(signals, *timeout)
	defer cancel()

	client, err := bess.Dial(*addr)
	if err != nil {
		return fmt.Errorf("dial %s: %w", *addr, err)
	}
	defer client.Close()

	cfg := bench.Config{Addr: *addr, Resource: *resource, Shapes: shapes,
		Seconds: *seconds, Preload: *preload, PktSize: *pktSize, Gates: *gates, Base: uint32(base)}
	if !*noGraph {
		cleanup, applyErr := bench.ApplyGraph(ctx, client, cfg, *workerCore, *force)
		if cleanup != nil {
			// Applying can fail after the daemon's own pipeline was cleared, so
			// the cleanup runs then too; if it fails, the run fails.
			defer func() {
				cctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 10*time.Second)
				defer cancel()
				if cerr := cleanup(cctx); cerr != nil {
					fmt.Fprintf(os.Stderr, "bessbench: %v\n", cerr)
					if err == nil {
						err = fmt.Errorf("the daemon's pipeline was not put back: %w", cerr)
					}
				}
			}()
		}
		if applyErr != nil {
			return applyErr
		}
		time.Sleep(bench.WaitSettled)
	}

	res, err := bench.Run(ctx, client, cfg)
	if err != nil {
		return err
	}
	floor, err := bench.Fit(res.Shapes)
	if err != nil {
		return err
	}
	if len(res.Shapes) < 3 {
		fmt.Fprintf(os.Stderr,
			"bessbench: warning: a line through %d shapes is thin -- the default sweep is four, %s each\n",
			len(res.Shapes), flag.Lookup("seconds").DefValue)
	}
	if *asJSON {
		return writeJSON(res, floor, samples)
	}
	return writeText(res, floor, samples)
}

func writeText(res bench.Result, floor bench.Floor, samples []bench.Sample) error {
	fmt.Printf("bessbench: %s on %s, %d rules preloaded, %s per shape\n\n",
		res.Config.Resource, res.Config.Addr, res.Config.Preload, res.Config.Seconds)
	fmt.Printf("%5s %9s %11s %11s\n", "ops", "samples", "p50", "p99")
	for _, s := range res.Shapes {
		fmt.Printf("%5d %9d %10.1fus %10.1fus\n", s.Ops, s.Samples, us(s.P50), us(s.P99))
	}
	fmt.Printf("\nfloor: %s (least squares over %s, p50)\n", floor, opsList(res.Shapes))
	fmt.Printf("busy answers: %.0f (the daemon retries those inside itself first)\n", res.Busy)
	if len(samples) > 0 {
		fmt.Printf("\nyour samples against it:\n%s", bench.Compare(floor, samples...))
		return nil
	}
	fmt.Printf("\nTime one of your own transactions, count the operations it sends, and pass the\n" +
		"pair as -compare 'label=ops:duration' to see how far it is from this floor.\n")
	return nil
}

// report is the JSON form: the sweep inlined beside the fitted floor and, when
// samples were given, the comparison. Durations are nanoseconds, as Go writes
// a time.Duration.
type report struct {
	bench.Result
	Floor    bench.Floor       `json:"floor"`
	Compared *bench.Comparison `json:"compared,omitempty"`
}

func writeJSON(res bench.Result, floor bench.Floor, samples []bench.Sample) error {
	out := report{Result: res, Floor: floor}
	if len(samples) > 0 {
		cmp := bench.Compare(floor, samples...)
		out.Compared = &cmp
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	return enc.Encode(out)
}

// shapeList parses the -shapes flag.
func shapeList(s string) ([]int, error) {
	var out []int
	for _, part := range strings.Split(s, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		n, err := strconv.Atoi(part)
		if err != nil {
			return nil, fmt.Errorf("-shapes %q: %w", s, err)
		}
		out = append(out, n)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("-shapes %q has no sizes in it", s)
	}
	return out, nil
}

// sampleList parses the -compare flag: label=ops:duration, comma separated. The
// label may be empty, in which case the operation count names the sample.
func sampleList(s string) ([]bench.Sample, error) {
	if strings.TrimSpace(s) == "" {
		return nil, nil
	}
	var out []bench.Sample
	for _, part := range strings.Split(s, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		label, rest := "", part
		if i := strings.Index(part, "="); i >= 0 {
			label, rest = part[:i], part[i+1:]
		}
		ops, latency, ok := strings.Cut(rest, ":")
		if !ok {
			return nil, fmt.Errorf("-compare %q: want label=ops:duration", part)
		}
		n, err := strconv.Atoi(strings.TrimSpace(ops))
		if err != nil {
			return nil, fmt.Errorf("-compare %q: %w", part, err)
		}
		d, err := time.ParseDuration(strings.TrimSpace(latency))
		if err != nil {
			return nil, fmt.Errorf("-compare %q: %w", part, err)
		}
		out = append(out, bench.Sample{Ops: n, Latency: d, Label: label})
	}
	return out, nil
}

func opsList(shapes []bench.Shape) string {
	parts := make([]string, 0, len(shapes))
	for _, s := range shapes {
		parts = append(parts, strconv.Itoa(s.Ops))
	}
	return strings.Join(parts, ", ") + " operations"
}

func us(d time.Duration) float64 { return float64(d) / float64(time.Microsecond) }
