// SPDX-License-Identifier: BSD-3-Clause

// Package bench measures the transaction floor of a running bessd through the
// SDK, and places a caller's own control path against it, so that a comparison
// is made against this daemon on this machine rather than against a number
// written down somewhere.
//
// The floor: a sweep churns a map resource -- by default one ExactMatch module's
// `em/rules` -- in transactions of several sizes, each transaction inserting as
// many rules as it removes so the table's size stays fixed and only the
// operation count varies. Latency against that count is a fixed cost per
// transaction plus a cost per operation, and those two numbers are what a
// caller needs: their own work has its own operation count, and dividing a
// latency by that count, or comparing against a differently shaped transaction,
// reads the fixed round trip as per-operation cost -- overstating it by an order
// of magnitude at these sizes.
//
// The whole thing in one call, for a caller who just wants the numbers:
//
//	res, floor, err := bench.Sweep(ctx, bench.Config{Addr: "localhost:10514"})
//
// And the same, inside a caller's own benchmark, measuring their path with the
// same normalisation:
//
//	var mine bench.Sample
//	b.Run("establish", func(b *testing.B) {
//		mine, err = bench.Measure(ctx, 11, b.N, "session establishment", func(ctx context.Context) error {
//			return host.Establish(ctx, ...)   // one transaction per call
//		})
//	})
//	cmp := bench.Compare(floor, mine)
//	b.ReportMetric(float64(cmp.Excess[0].PerOp)/1000, "us/op-over-floor")
//
// The results carry JSON tags with durations in nanoseconds, so a caller's
// tooling can read a sweep as well as run one.
//
// The daemon needs a pipeline with an ExactMatch module named `em` whose gates
// 0..Config.Gates are wired, and the client should be on another core: a
// dataplane worker sharing a core with the daemon's control thread triples the
// 99th percentile and halves the packet rate, so a floor measured that way is
// not a floor.
package bench

import (
	"context"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
	"time"

	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	"google.golang.org/protobuf/types/dynamicpb"

	bess "github.com/krsna1729/bess/sdk/go/bess"
)

// LiveAddrs is how many addresses cycle. A run's erase pool is the addresses
// behind the insert cursor, so the two never meet as long as the preload plus
// the largest transaction stays under this.
const LiveAddrs = 4096

// Config names what to measure. The zero value is usable with Addr set.
type Config struct {
	Addr     string        `json:"addr"`               // the daemon's control endpoint
	Resource string        `json:"resource,omitempty"` // the map to churn; default em/rules
	Shapes   []int         `json:"shapes,omitempty"`   // operations per transaction, even; default 2, 8, 16, 32
	Seconds  time.Duration `json:"seconds,omitempty"`  // each shape's window; default 2s
	Preload  int           `json:"preload,omitempty"`  // rules loaded before measuring; default 1000
	PktSize  int           `json:"pkt_size,omitempty"` // the bench graph's packet size; default 60
	Gates    int           `json:"gates,omitempty"`    // gates rules steer to; default 4
	Base     uint32        `json:"base,omitempty"`     // first key address; default 0x0b000000
	// ApplyGraph makes Sweep apply the bench graph rather than use one that is
	// already there, on WorkerCore. It is the same safe path the command takes:
	// a daemon with a pipeline is refused rather than replaced.
	ApplyGraph bool `json:"apply_graph,omitempty"`
	WorkerCore int  `json:"worker_core,omitempty"` // that graph's worker CPU; default 2, never the control CPU
}

// WithDefaults fills in what a caller left out, so a caller can spread the
// defaults they were measured with into their own report.
func (c Config) WithDefaults() Config {
	if c.Resource == "" {
		c.Resource = "em/rules"
	}
	if len(c.Shapes) == 0 {
		c.Shapes = []int{2, 8, 16, 32}
	}
	if c.Seconds == 0 {
		c.Seconds = 2 * time.Second
	}
	if c.Preload == 0 {
		c.Preload = 1000
	}
	if c.Gates == 0 {
		c.Gates = 4
	}
	if c.PktSize == 0 {
		c.PktSize = 60
	}
	if c.WorkerCore == 0 {
		c.WorkerCore = 2
	}
	if c.Base == 0 {
		c.Base = 0x0b000000
	}
	return c
}

// Shape is one transaction size's measurement. Durations are nanoseconds in
// JSON, as Go writes a time.Duration.
type Shape struct {
	Ops     int           `json:"ops"`
	Samples int           `json:"samples"`
	P50     time.Duration `json:"p50_ns"`
	P99     time.Duration `json:"p99_ns"`
}

// Result is a completed sweep: what was configured, what came back, and the
// daemon's final busy answers over the run.
type Result struct {
	Config Config  `json:"config"`
	Shapes []Shape `json:"shapes"`
	Busy   float64 `json:"busy"`
}

// Floor is the fitted latency model: what a transaction costs whatever it
// carries, and what each operation adds. A busy answer is retried inside the
// daemon first, for up to about 6 ms, so a non-zero Busy means samples hide
// retries rather than that anything failed.
type Floor struct {
	Fixed time.Duration `json:"fixed_ns"`
	PerOp time.Duration `json:"per_op_ns"`
}

func (f Floor) String() string {
	return fmt.Sprintf("%.1fus fixed + %.2fus per operation", us(f.Fixed), us(f.PerOp))
}

// Dial opens a client for the configured endpoint, with this SDK's defaults.
func Dial(cfg Config) (*bess.Client, error) {
	client, err := bess.Dial(cfg.Addr)
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", cfg.Addr, err)
	}
	return client, nil
}

// joinCleanup folds a graph cleanup's failure into the error the run is
// returning. Both are kept: the cleanup's says what state the daemon was left
// in, the run's says why. Nothing is written about a cleanup that succeeded.
func joinCleanup(cerr, err error) error {
	if cerr == nil {
		return err
	}
	return errors.Join(err, fmt.Errorf("removing the bench graph: %w", cerr))
}

// Sweep is the whole measurement in one call: dial, churn every shape, fit the
// line, and close the client. Config.Addr must be set.
func Sweep(ctx context.Context, cfg Config) (res Result, floor Floor, err error) {
	cfg = cfg.WithDefaults()
	client, err := Dial(cfg)
	if err != nil {
		return Result{}, Floor{}, err
	}
	// A Close that fails has nothing a measurement can act on: the numbers are
	// already in hand.
	defer func() { _ = client.Close() }()
	if cfg.ApplyGraph {
		var cleanup func(context.Context) error
		cleanup, err = ApplyGraph(ctx, client, cfg, cfg.WorkerCore, false)
		if cleanup != nil {
			// The cleanup comes back even with an error, because the graph may
			// be on the daemon; a caller has to be able to tell if it fails.
			// The named result is assigned here, which is why cleanup is
			// declared above: a short declaration would shadow the error and
			// the failure would never reach the caller.
			defer func() { err = joinCleanup(cleanup(context.WithoutCancel(ctx)), err) }()
		}
		if err != nil {
			return Result{}, Floor{}, err
		}
		time.Sleep(WaitSettled)
	}
	res, err = Run(ctx, client, cfg)
	if err != nil {
		return Result{}, Floor{}, err
	}
	floor, err = Fit(res.Shapes)
	if err != nil {
		return Result{}, Floor{}, err
	}
	return res, floor, nil
}

// Run churns the configured resource at each shape and returns the samples. The
// client must already be dialled and the daemon must have the resource.
func Run(ctx context.Context, c *bess.Client, cfg Config) (Result, error) {
	cfg = cfg.WithDefaults()
	rules, err := c.Resource(ctx, cfg.Resource)
	if err != nil {
		return Result{}, fmt.Errorf("%s: %w (is a pipeline with that module applied?)", cfg.Resource, err)
	}
	keys, values, err := messages(ctx, c, rules, cfg)
	if err != nil {
		return Result{}, err
	}
	// The erase pool exists before the first measured transaction, so the keys
	// a transaction erases are never the ones it inserts.
	preload := c.Transaction()
	for i := 0; i < cfg.Preload; i++ {
		if err := preload.Upsert(rules, keys[i], values[i]); err != nil {
			return Result{}, fmt.Errorf("preload: %w", err)
		}
	}
	if _, err := preload.Commit(ctx); err != nil {
		return Result{}, fmt.Errorf("preload commit: %w", err)
	}

	busyBefore := busyAnswers(ctx, c)
	out := Result{Config: cfg, Shapes: make([]Shape, 0, len(cfg.Shapes))}
	next, oldest := cfg.Preload, 0
	for _, ops := range cfg.Shapes {
		if ops <= 0 || ops%2 != 0 {
			return Result{}, fmt.Errorf("shapes must be positive and even: an odd transaction grows the table, which is a different measurement (got %d)", ops)
		}
		k := ops / 2
		samples, err := churn(ctx, c, rules, keys, values, k, cfg, &next, &oldest)
		if err != nil {
			return Result{}, err
		}
		sort.Slice(samples, func(i, j int) bool { return samples[i] < samples[j] })
		out.Shapes = append(out.Shapes, Shape{
			Ops:     ops,
			Samples: len(samples),
			P50:     samples[len(samples)/2],
			P99:     samples[min(len(samples)-1, len(samples)*99/100)],
		})
	}
	out.Busy = busyAnswers(ctx, c) - busyBefore
	return out, nil
}

// churn runs one shape's window, timing Commit alone as the C++ bench does: what
// a caller waits for is the commit, and building it is theirs to keep short.
func churn(ctx context.Context, c *bess.Client, rules bess.Resource, keys, values []proto.Message,
	k int, cfg Config, next, oldest *int) ([]time.Duration, error) {
	deadline := time.Now().Add(cfg.Seconds)
	var samples []time.Duration
	for time.Now().Before(deadline) {
		tx := c.Transaction()
		for i := 0; i < k; i++ {
			idx := (*next + i) % LiveAddrs
			if err := tx.Upsert(rules, keys[idx], values[idx]); err != nil {
				return nil, err
			}
		}
		for i := 0; i < k; i++ {
			idx := (*oldest + i) % LiveAddrs
			if err := tx.Erase(rules, keys[idx]); err != nil {
				return nil, err
			}
		}
		start := time.Now()
		if _, err := tx.Commit(ctx); err != nil {
			return nil, err
		}
		samples = append(samples, time.Since(start))
		*next += k
		*oldest += k
	}
	if len(samples) == 0 {
		return nil, fmt.Errorf("no samples: %s is too short a window", cfg.Seconds)
	}
	return samples, nil
}

// Fit is the least-squares line through the shapes: the fixed cost of a
// transaction and the cost of each operation it carries. Two shapes give a line
// and more give one that a single noisy shape cannot move much, which is why the
// default sweep has four.
func Fit(shapes []Shape) (Floor, error) {
	if len(shapes) < 2 {
		return Floor{}, fmt.Errorf("a line needs at least two shapes, got %d", len(shapes))
	}
	var sx, sy, sxx, sxy, sw float64
	for _, s := range shapes {
		x, y := float64(s.Ops), us(s.P50)
		sx += x
		sy += y
		sxx += x * x
		sxy += x * y
		sw++
	}
	den := sw*sxx - sx*sx
	if math.Abs(den) < 1e-9 {
		return Floor{}, fmt.Errorf("the shapes %v have no spread in operation count", shapeOps(shapes))
	}
	perOp := (sw*sxy - sx*sy) / den
	fixed := (sy - perOp*sx) / sw
	if perOp < 0 {
		return Floor{}, fmt.Errorf("the shapes %v are faster the more they carry, which is noise and not a line", shapeOps(shapes))
	}
	return Floor{
		Fixed: time.Duration(fixed * float64(time.Microsecond)),
		PerOp: time.Duration(perOp * float64(time.Microsecond)),
	}, nil
}

// Sample is one measurement of a caller's own path: how many daemon-side
// operations the transaction it makes carries, and how long that took.
type Sample struct {
	Ops     int           `json:"ops"`
	Latency time.Duration `json:"latency_ns"`
	Label   string        `json:"label,omitempty"`
	Samples int           `json:"samples,omitempty"` // how many runs Latency is the median of
}

// Measure runs f n times and returns the median as a sample of the caller's own
// path, so that their measurement is normalised the same way the floor's was.
// f should make one transaction per call; what it does between calls is not
// measured. ops is how many daemon-side operations that transaction carries.
func Measure(ctx context.Context, ops, n int, label string, f func(context.Context) error) (Sample, error) {
	if ops <= 0 {
		return Sample{}, fmt.Errorf("ops must be positive, got %d", ops)
	}
	if n <= 0 {
		return Sample{}, fmt.Errorf("n must be positive, got %d", n)
	}
	durs := make([]time.Duration, 0, n)
	for i := 0; i < n; i++ {
		start := time.Now()
		if err := f(ctx); err != nil {
			return Sample{}, err
		}
		durs = append(durs, time.Since(start))
	}
	sort.Slice(durs, func(i, j int) bool { return durs[i] < durs[j] })
	return Sample{Ops: ops, Latency: durs[len(durs)/2], Label: label, Samples: n}, nil
}

// Comparison is where a caller's path sits against a floor.
type Comparison struct {
	Floor     Floor         `json:"floor"`
	Excess    []Excess      `json:"excess,omitempty"`
	OwnFixed  time.Duration `json:"own_fixed_ns,omitempty"`
	OwnPerOp  time.Duration `json:"own_per_op_ns,omitempty"`
	HasFit    bool          `json:"has_fit,omitempty"`
	PerOpHigh bool          `json:"per_op_above_floor,omitempty"`
	// OwnLineRejected says the caller gave two or more samples but they do not
	// describe one path at two sizes, so only the per-sample excesses stand.
	OwnLineRejected bool `json:"own_line_rejected,omitempty"`
}

// Excess is one sample against the floor. PerOp is what the sample implies per
// operation once the floor's fixed cost is subtracted; a caller's own fixed cost
// cannot be seen in a single sample, so that subtraction uses the floor's.
type Excess struct {
	Sample    Sample        `json:"sample"`
	PerOp     time.Duration `json:"per_op_ns"`
	OverFloor time.Duration `json:"over_floor_ns"`
	Multiple  float64       `json:"multiple,omitempty"` // PerOp / the floor's per-operation cost
}

// Compare normalises a caller's measurements against the floor. One sample gives
// the multiple directly; two or more also fit their own line, whose slope is the
// comparison that does not assume the floor's fixed cost applies to another
// stack.
func Compare(floor Floor, samples ...Sample) Comparison {
	out := Comparison{Floor: floor}
	shapes := make([]Shape, 0, len(samples))
	for _, s := range samples {
		if s.Ops <= 0 {
			continue
		}
		perOp := (s.Latency - floor.Fixed) / time.Duration(s.Ops)
		e := Excess{Sample: s, PerOp: perOp, OverFloor: perOp - floor.PerOp}
		if floor.PerOp > 0 {
			e.Multiple = float64(perOp) / float64(floor.PerOp)
		}
		out.Excess = append(out.Excess, e)
		shapes = append(shapes, Shape{Ops: s.Ops, P50: s.Latency})
	}
	// A line through the caller's samples is only a line if it describes one
	// path at two sizes. A fixed part at or below zero means it does not: the
	// samples are different operations, and their per-sample excesses above are
	// all that can be read from them.
	if len(shapes) >= 2 {
		if own, err := Fit(shapes); err == nil && own.Fixed > 0 {
			out.OwnFixed, out.OwnPerOp, out.HasFit = own.Fixed, own.PerOp, true
			out.PerOpHigh = own.PerOp > floor.PerOp
		} else {
			out.OwnLineRejected = true
		}
	}
	return out
}

// String is the report a caller reads: their samples normalised against the
// floor, and, when two or more were given, the slope of their own line.
func (c Comparison) String() string {
	var b strings.Builder
	fmt.Fprintf(&b, "floor: %s\n", c.Floor)
	for _, e := range c.Excess {
		what := e.Sample.Label
		if what == "" {
			what = fmt.Sprintf("%d operations", e.Sample.Ops)
		}
		samples := ""
		if e.Sample.Samples > 1 {
			samples = fmt.Sprintf(" (median of %d)", e.Sample.Samples)
		}
		fmt.Fprintf(&b, "  %-28s %-9s%s %.1fus/op, %.1fx the floor (%+.1fus/op)\n",
			what, dur(e.Sample.Latency), samples, us(e.PerOp), e.Multiple, us(e.OverFloor))
	}
	if c.HasFit {
		verb := "below"
		if c.PerOpHigh {
			verb = "above"
		}
		fmt.Fprintf(&b, "  their own line: %.1fus fixed + %.2fus per operation, %.2fx %s the floor's per-operation cost\n",
			us(c.OwnFixed), us(c.OwnPerOp),
			float64(c.OwnPerOp)/float64(c.Floor.PerOp), verb)
	} else if c.OwnLineRejected {
		fmt.Fprintf(&b, "  no line for the caller: these samples are not one operation at two sizes, so\n"+
			"  only the excesses above stand (measure the same operation at two or more sizes for a line)\n")
	}
	return b.String()
}

// messages builds one key and one value per address, from the daemon's own
// schema, so the measured loop pays nothing for building them and every client
// measures the same shape.
func messages(ctx context.Context, c *bess.Client, rules bess.Resource, cfg Config) ([]proto.Message, []proto.Message, error) {
	sc, err := c.Schema(ctx)
	if err != nil {
		return nil, nil, fmt.Errorf("schema: %w", err)
	}
	files, err := protodesc.NewFiles(sc.GetDescriptors())
	if err != nil {
		return nil, nil, fmt.Errorf("the daemon's descriptors: %w", err)
	}
	keyMD, err := message(files, protoreflect.FullName(rules.KeyType))
	if err != nil {
		return nil, nil, err
	}
	valueMD, err := message(files, protoreflect.FullName(rules.ValueType))
	if err != nil {
		return nil, nil, err
	}
	if cfg.Preload+maxShape(cfg.Shapes) > LiveAddrs {
		return nil, nil, fmt.Errorf("preload %d plus the largest shape %d exceeds the %d addresses that cycle",
			cfg.Preload, maxShape(cfg.Shapes), LiveAddrs)
	}
	keys := make([]proto.Message, LiveAddrs)
	values := make([]proto.Message, LiveAddrs)
	for i := range keys {
		var raw [4]byte
		binary.BigEndian.PutUint32(raw[:], cfg.Base+uint32(i))
		key := dynamicpb.NewMessage(keyMD)
		keyJSON := `{"fields": [{"value_bin": "` + base64.StdEncoding.EncodeToString(raw[:]) + `"}]}`
		if err := protojson.Unmarshal([]byte(keyJSON), key); err != nil {
			return nil, nil, fmt.Errorf("key %d: %w", i, err)
		}
		keys[i] = key
		value := dynamicpb.NewMessage(valueMD)
		valueJSON := fmt.Sprintf(`{"gate": %d}`, 1+i%cfg.Gates)
		if err := protojson.Unmarshal([]byte(valueJSON), value); err != nil {
			return nil, nil, fmt.Errorf("value %d: %w", i, err)
		}
		values[i] = value
	}
	return keys, values, nil
}

func message(files *protoregistry.Files, name protoreflect.FullName) (protoreflect.MessageDescriptor, error) {
	d, err := files.FindDescriptorByName(name)
	if err != nil {
		return nil, fmt.Errorf("the daemon's schema has no %s: %w", name, err)
	}
	md, ok := d.(protoreflect.MessageDescriptor)
	if !ok {
		return nil, fmt.Errorf("%s is not a message", name)
	}
	return md, nil
}

// busyAnswers is the daemon's count of final busy answers so far. A zero does
// not mean no busy was met: the daemon retries those inside itself first.
func busyAnswers(ctx context.Context, c *bess.Client) float64 {
	samples, err := c.Metrics(ctx)
	if err != nil {
		return 0
	}
	for _, s := range samples {
		if l := s.GetLabels(); s.GetName() == "bess_transactions_total" && l["outcome"] == "busy" {
			return s.GetValue()
		}
	}
	return 0
}

func maxShape(shapes []int) int {
	max := 0
	for _, s := range shapes {
		if s > max {
			max = s
		}
	}
	return max
}

func shapeOps(shapes []Shape) []int {
	out := make([]int, 0, len(shapes))
	for _, s := range shapes {
		out = append(out, s.Ops)
	}
	return out
}

func us(d time.Duration) float64 { return float64(d) / float64(time.Microsecond) }

func dur(d time.Duration) string { return fmt.Sprintf("%.0fus", us(d)) }
