// SPDX-License-Identifier: BSD-3-Clause
package bench

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"sort"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
	"google.golang.org/protobuf/types/known/anypb"

	bess "github.com/krsna1729/bess/sdk/go/bess"
	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// StageConfig selects replacements across a prefix of eight independently
// resource-bearing ExactMatch modules. Every module retains Resident entries.
// Message mode measures SDK packing; encoded mode borrows pre-encoded bytes.
// Both use public transactions, epoch checks, and individual commit receipts.
type StageConfig struct {
	Counts     []int  `json:"counts"`
	Iterations int    `json:"iterations"`
	Mode       string `json:"mode"`
	Locality   string `json:"locality"`
	Resident   int    `json:"resident_per_stage"`
	// Zero selects one caller. Parallel callers need disjoint spread-key lanes.
	Concurrency int  `json:"concurrency"`
	MeasureAcks bool `json:"measure_acks"`
	WorkerCore  int  `json:"worker_core"`
	Force       bool `json:"force"`
	NoGraph     bool `json:"no_graph"`
	GraphOnly   bool `json:"graph_only"`
	KeepGraph   bool `json:"keep_graph"`
}

type StageBlock struct {
	Transactions     int     `json:"transactions"`
	WallNS           int64   `json:"wall_ns"`
	ProcessCPUNS     int64   `json:"client_process_cpu_ns"`
	MeanWallNS       float64 `json:"mean_wall_ns"`
	MeanProcessCPUNS float64 `json:"mean_client_process_cpu_ns"`
	// CLOCK_MONOTONIC bounds correlate perf samples without per-call observers.
	StartedMonotonicNS int64 `json:"started_monotonic_ns"`
	EndedMonotonicNS   int64 `json:"ended_monotonic_ns"`
}

// StageOutcome has no per-call clock observer. The enclosing block is timed.
// A failed attempt is retained before RunStages returns its error.
type StageOutcome struct {
	RequestID string                `json:"request_id"`
	Phase     string                `json:"phase,omitempty"`
	Ordinal   int                   `json:"ordinal"`
	Slot      int                   `json:"slot"`
	Gate      uint32                `json:"gate"`
	Applied   *bess.Applied         `json:"applied,omitempty"`
	Record    *pb.TransactionRecord `json:"record,omitempty"`
	ErrorType string                `json:"error_type,omitempty"`
	Error     string                `json:"error,omitempty"`
}

type StageWorkerOutcome struct {
	Accepted int          `json:"accepted"`
	Last     StageOutcome `json:"last"`
}

type StageMeasurement struct {
	Stages             int                  `json:"stages"`
	OpsPerTransaction  int                  `json:"ops_per_transaction"`
	KeyValueBytes      int                  `json:"key_value_bytes_per_transaction"`
	RequestBytes       int                  `json:"request_bytes_example"`
	Build              StageBlock           `json:"build"`
	Commit             StageBlock           `json:"commit"`
	SeedReceipts       []StageOutcome       `json:"seed_receipts"`
	Cold               StageOutcome         `json:"cold"`
	ColdCommit         StageBlock           `json:"cold_commit"`
	Last               StageOutcome         `json:"last"`
	BuildCommit        StageBlock           `json:"build_commit"`
	CombinedCold       StageOutcome         `json:"combined_cold"`
	CombinedLast       StageOutcome         `json:"combined_last"`
	CommitWorkers      []StageWorkerOutcome `json:"commit_workers,omitempty"`
	CombinedWorkers    []StageWorkerOutcome `json:"combined_workers,omitempty"`
	CommitAckLatency   *Shape               `json:"commit_ack_latency,omitempty"`
	CombinedAckLatency *Shape               `json:"combined_ack_latency,omitempty"`
}

type StageResult struct {
	Config          StageConfig        `json:"config"`
	Resources       []string           `json:"resources"`
	DaemonEpoch     uint64             `json:"daemon_epoch"`
	SchemaSHA256    string             `json:"schema_sha256"`
	Concurrency     int                `json:"concurrency"`
	RetainedStages  int                `json:"retained_stages"`
	TargetEntries   int                `json:"target_entries"`
	RetainedEntries int                `json:"retained_entries"`
	Consistency     string             `json:"consistency"`
	KeyHex          string             `json:"key_slot0_hex"`
	Gate1Hex        string             `json:"value_gate1_hex"`
	Gate2Hex        string             `json:"value_gate2_hex"`
	CapacityPolicy  string             `json:"capacity_policy"`
	Boundaries      string             `json:"boundaries"`
	Measurements    []StageMeasurement `json:"measurements"`
}

func stagePipeline(stages int, arg func(string, map[string]any) (*anypb.Any, error)) (*pb.Pipeline, error) {
	if stages != 8 {
		return nil, errors.New("stage fixture requires all eight modules regardless of modified prefix")
	}
	init, err := arg("ExactMatch", map[string]any{"fields": []any{map[string]any{"offset": 26, "num_bytes": 4}}})
	if err != nil {
		return nil, err
	}
	p := &pb.Pipeline{}
	for i := range stages {
		name := fmt.Sprintf("stage%d", i)
		p.Modules = append(p.Modules, &pb.Module{Name: name, Mclass: "ExactMatch", Arg: init})
		next := "stage_sink"
		if i+1 < stages {
			next = fmt.Sprintf("stage%d", i+1)
		}
		for gate := range 3 {
			p.Connections = append(p.Connections, &pb.Connection{Upstream: name, Ogate: uint32(gate), Downstream: next})
		}
	}
	p.Modules = append(p.Modules, &pb.Module{Name: "stage_sink", Mclass: "Sink"})
	return p, nil
}

func stageSelection(ordinal, resident int, locality string) (int, uint32) {
	working := resident
	if locality == "hot" {
		working = 1
	}
	return ordinal % working, uint32(2 - (ordinal/working)%2)
}

func stageCPU() (int64, error) {
	var u syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &u); err != nil {
		return 0, err
	}
	return u.Utime.Nano() + u.Stime.Nano(), nil
}

func stageBlock(run func() (int, error)) (StageBlock, error) {
	cpu, err := stageCPU()
	if err != nil {
		return StageBlock{}, err
	}
	var started, ended unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &started); err != nil {
		return StageBlock{}, err
	}
	start := time.Now()
	n, runErr := run()
	end := time.Now()
	clockErr := unix.ClockGettime(unix.CLOCK_MONOTONIC, &ended)
	wall := end.Sub(start)
	endCPU, cpuErr := stageCPU()
	b := StageBlock{Transactions: n, WallNS: wall.Nanoseconds(), ProcessCPUNS: endCPU - cpu,
		StartedMonotonicNS: started.Nano(), EndedMonotonicNS: ended.Nano()}
	if n > 0 {
		b.MeanWallNS = float64(b.WallNS) / float64(n)
		b.MeanProcessCPUNS = float64(b.ProcessCPUNS) / float64(n)
	}
	return b, errors.Join(runErr, cpuErr, clockErr)
}

func stageOutcome(tx *bess.Transaction, ordinal, resident int, locality string, applied *bess.Applied, err error) StageOutcome {
	slot, gate := stageSelection(ordinal, resident, locality)
	o := StageOutcome{RequestID: tx.RequestID, Ordinal: ordinal, Slot: slot, Gate: gate, Applied: applied}
	if err != nil {
		o.ErrorType, o.Error = fmt.Sprintf("%T", err), err.Error()
		var rejected *bess.RejectedError
		var conflict *bess.ConflictError
		if errors.As(err, &rejected) {
			o.Record = rejected.Record
		} else if errors.As(err, &conflict) {
			o.Record = conflict.Record
		}
	}
	return o
}

func stageCommit(ctx context.Context, tx *bess.Transaction, operations int, epoch uint64) (*bess.Applied, error) {
	applied, err := tx.Commit(ctx)
	if err != nil {
		return applied, err
	}
	if applied == nil || applied.Record == nil {
		return applied, errors.New("stage commit returned no individual receipt")
	}
	rec := applied.Record
	if applied.RequestID != tx.RequestID || rec.GetRequestId() != tx.RequestID ||
		applied.DaemonEpoch != epoch || applied.Replayed || rec.GetOutcome() != pb.TransactionRecord_OUTCOME_APPLIED ||
		len(rec.GetOps()) != operations {
		return applied, fmt.Errorf("stage commit receipt does not match request %s with %d operations under epoch %d", tx.RequestID, operations, epoch)
	}
	for i, op := range rec.GetOps() {
		if op.GetStatus() != pb.TransactionOpResult_STATUS_APPLIED {
			return applied, fmt.Errorf("stage commit %s operation %d was not applied", tx.RequestID, i)
		}
	}
	return applied, nil
}

const maxStageIterations = 65536

func validateStageConfig(cfg StageConfig) error {
	if cfg.Iterations < 1 || cfg.Iterations > maxStageIterations || cfg.Resident < 1 || cfg.Resident > 32768 || len(cfg.Counts) == 0 {
		return errors.New("stages require 1..65536 iterations, 1..32768 resident entries and nonempty counts")
	}
	concurrency := cfg.Concurrency
	if concurrency == 0 {
		concurrency = 1
	}
	if concurrency < 1 || concurrency > 64 || concurrency&(concurrency-1) != 0 ||
		concurrency > cfg.Iterations || cfg.Resident%concurrency != 0 ||
		(concurrency > 1 && cfg.Locality != "spread") {
		return errors.New("stage concurrency must be a power of two in 1..64, no greater than iterations, divide resident entries, and use spread keys when parallel")
	}
	if cfg.Mode != "encoded" && cfg.Mode != "message" {
		return errors.New("stage-mode must be encoded or message")
	}
	if cfg.Locality != "hot" && cfg.Locality != "spread" {
		return errors.New("stage-locality must be hot or spread")
	}
	for _, n := range cfg.Counts {
		if n != 1 && n != 2 && n != 3 && n != 4 && n != 8 {
			return fmt.Errorf("stage count %d is not one of 1,2,3,4,8", n)
		}
	}
	return nil
}

// An attached table is either empty or the complete replacement fixture.
// Compare encoded keys, not slot indices or just entry counts; never erase
// entries that belong to the caller in order to make the fixture fit.
func stageResidentCount(contents *pb.ResourceContents, expected map[string]struct{}, values [][]byte) (int, error) {
	entries := contents.GetEntries()
	if len(entries) != 0 && len(entries) != len(expected) {
		return 0, fmt.Errorf("%s has %d entries; expected empty or exactly %d", contents.GetName(), len(entries), len(expected))
	}
	seen := make(map[string]struct{}, len(entries))
	for _, entry := range entries {
		key, value := entry.GetKey(), entry.GetValue()
		if key == nil || key.GetTypeUrl() != "type.googleapis.com/bess.pb.ExactMatchRuleKey" ||
			value == nil || value.GetTypeUrl() != "type.googleapis.com/bess.pb.ExactMatchRuleValue" {
			return 0, fmt.Errorf("%s has incompatible entry types", contents.GetName())
		}
		k := string(key.GetValue())
		if _, ok := expected[k]; !ok {
			return 0, fmt.Errorf("%s has an unexpected resident key %x", contents.GetName(), key.GetValue())
		}
		if _, duplicate := seen[k]; duplicate {
			return 0, fmt.Errorf("%s has a duplicate resident key %x", contents.GetName(), key.GetValue())
		}
		if !bytes.Equal(value.GetValue(), values[1]) && !bytes.Equal(value.GetValue(), values[2]) {
			return 0, fmt.Errorf("%s has an incompatible resident value for key %x", contents.GetName(), key.GetValue())
		}
		seen[k] = struct{}{}
	}
	return len(entries), nil
}

// Strided lanes own disjoint keys when concurrency divides the resident set.
// Each lane waits for its individual receipt before revisiting any of its keys.
func stageWindow(ctx context.Context, iterations int, workers []StageWorkerOutcome,
	run func(context.Context, int) (StageOutcome, error)) (int, error) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	errs := make([]error, len(workers))
	var wg sync.WaitGroup
	for worker := range workers {
		wg.Go(func() {
			for ordinal := worker + 1; ordinal <= iterations; ordinal += len(workers) {
				if err := ctx.Err(); err != nil {
					errs[worker] = err
					return
				}
				outcome, err := run(ctx, ordinal)
				workers[worker].Last = outcome
				if err != nil {
					errs[worker] = err
					cancel()
					return
				}
				workers[worker].Accepted++
			}
		})
	}
	wg.Wait()
	accepted := 0
	for _, worker := range workers {
		accepted += worker.Accepted
	}
	return accepted, errors.Join(errs...)
}

func stageLast(workers []StageWorkerOutcome) (last StageOutcome) {
	for _, worker := range workers {
		if worker.Last.Ordinal > last.Ordinal {
			last = worker.Last
		}
	}
	return last
}

func stageAckSummary(samples []time.Duration, operations int) *Shape {
	observed := samples[:0]
	for _, sample := range samples {
		if sample > 0 {
			observed = append(observed, sample)
		}
	}
	if len(observed) == 0 {
		return nil
	}
	sort.Slice(observed, func(i, j int) bool { return observed[i] < observed[j] })
	return &Shape{Ops: operations, Samples: len(observed),
		P50: observed[len(observed)/2],
		P99: observed[min(len(observed)-1, len(observed)*99/100)]}
}

// RunStages creates one idle eight-stage graph, seeds all stages before each
// prefix measurement, and stops on any failed/unknown outcome. Setup, schema,
// message creation, wire encoding and the first post-seed transaction are outside
// the measured blocks. Build includes NewTransaction and Upsert[Encoded]; Commit
// includes public receipt validation, but not transaction build. Only cold/last
// client receipts are retained; the daemon keeps its normal bounded receipt ledger.
// CPU is this Go client process only; remote daemon CPU must be collected apart.
func RunStages(ctx context.Context, c *bess.Client, cfg StageConfig) (result StageResult, err error) {
	if cfg.Concurrency == 0 {
		cfg.Concurrency = 1
	}
	result.Config = cfg
	result.Concurrency = cfg.Concurrency
	result.Consistency = "REFERENTIAL"
	result.CapacityPolicy = "ExactMatch public default CapacityFor/autogrowth; no capacity argument exists; all eight stages seeded identically"
	result.Boundaries = "idle graph, one controller with bounded callers; no active packet reader; separate serial build, prebuilt public Commit, and freshly reseeded combined build+Commit; block wall/op is inverse throughput, not concurrent request latency; no per-call clocks; client-process CPU only; cold and all setup excluded"
	if cfg.MeasureAcks {
		result.Boundaries = "diagnostic per-Commit clocks include SDK retries and receipt validation; combined build excluded from acknowledgement clocks; block wall/op remains inverse throughput, not request latency; client-process CPU only; cold and setup excluded; corroborate with otherwise identical plain runs"
	}
	if err = validateStageConfig(cfg); err != nil {
		return result, err
	}
	result.TargetEntries = 8 * cfg.Resident
	if !cfg.NoGraph {
		cleanup, applyErr := ApplyGraph(ctx, c, Config{Stages: 8}, cfg.WorkerCore, cfg.Force)
		if cleanup != nil && (applyErr != nil || (!cfg.GraphOnly && !cfg.KeepGraph)) {
			defer func() {
				cctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 10*time.Second)
				defer cancel()
				err = errors.Join(err, cleanup(cctx))
			}()
		}
		if applyErr != nil {
			return result, applyErr
		}
	}
	schema, err := c.Schema(ctx)
	if err != nil {
		return result, err
	}
	descriptorWire, err := proto.MarshalOptions{Deterministic: true}.Marshal(schema.GetDescriptors())
	if err != nil {
		return result, err
	}
	schemaHash := sha256.Sum256(descriptorWire)
	result.SchemaSHA256 = hex.EncodeToString(schemaHash[:])
	files, err := protodesc.NewFiles(schema.GetDescriptors())
	if err != nil {
		return result, err
	}
	resources := make([]bess.Resource, 8)
	for i := range resources {
		name := fmt.Sprintf("stage%d/rules", i)
		r, rerr := c.Resource(ctx, name)
		if rerr != nil {
			return result, rerr
		}
		if r.Kind != pb.TransactionResource_KIND_MAP || r.KeyType != "bess.pb.ExactMatchRuleKey" || r.ValueType != "bess.pb.ExactMatchRuleValue" {
			return result, fmt.Errorf("%s does not match the ExactMatch map fixture", name)
		}
		if r.DaemonEpoch == 0 || (i > 0 && r.DaemonEpoch != resources[0].DaemonEpoch) {
			return result, fmt.Errorf("%s does not share the stage fixture daemon epoch", name)
		}
		resources[i] = r
		result.Resources = append(result.Resources, name)
	}
	result.DaemonEpoch = resources[0].DaemonEpoch
	kmd, err := message(files, protoreflect.FullName(resources[0].KeyType))
	if err != nil {
		return result, err
	}
	vmd, err := message(files, protoreflect.FullName(resources[0].ValueType))
	if err != nil {
		return result, err
	}
	keys := make([]proto.Message, cfg.Resident)
	keyWire := make([][]byte, cfg.Resident)
	for i := range keys {
		var raw [4]byte
		binary.BigEndian.PutUint32(raw[:], 0x0b000001+uint32(i))
		m := dynamicpb.NewMessage(kmd)
		fd := kmd.Fields().ByName("fields")
		if fd == nil || !fd.IsList() || fd.Message() == nil {
			return result, errors.New("ExactMatch key has no repeated fields message")
		}
		elem := dynamicpb.NewMessage(fd.Message())
		bin := elem.Descriptor().Fields().ByName("value_bin")
		if bin == nil || bin.Kind() != protoreflect.BytesKind {
			return result, errors.New("ExactMatch field has no bytes value_bin")
		}
		elem.Set(bin, protoreflect.ValueOfBytes(append([]byte(nil), raw[:]...)))
		m.Mutable(fd).List().Append(protoreflect.ValueOfMessage(elem))
		keys[i] = m
		keyWire[i], err = proto.Marshal(m)
		if err != nil {
			return result, err
		}
	}
	values := make([]proto.Message, 3)
	valueWire := make([][]byte, 3)
	gateField := vmd.Fields().ByName("gate")
	if gateField == nil || gateField.Kind() != protoreflect.Uint64Kind {
		return result, errors.New("ExactMatch value has no uint64 gate")
	}
	for gate := 1; gate <= 2; gate++ {
		m := dynamicpb.NewMessage(vmd)
		m.Set(gateField, protoreflect.ValueOfUint64(uint64(gate)))
		values[gate] = m
		valueWire[gate], err = proto.Marshal(m)
		if err != nil {
			return result, err
		}
	}
	result.KeyHex = hex.EncodeToString(keyWire[0])
	result.Gate1Hex, result.Gate2Hex = hex.EncodeToString(valueWire[1]), hex.EncodeToString(valueWire[2])
	expected := make(map[string]struct{}, len(keyWire))
	for _, key := range keyWire {
		expected[string(key)] = struct{}{}
	}
	snapshot, err := c.Pipeline(ctx, bess.WithResources(result.Resources...))
	if err != nil {
		return result, err
	}
	if snapshot.Version.DaemonEpoch != result.DaemonEpoch {
		return result, errors.New("stage snapshot daemon epoch does not match resources")
	}
	if len(snapshot.Workers) != 0 || len(snapshot.Pipeline.GetWorkers()) != 0 || len(snapshot.Pipeline.GetTasks()) != 0 {
		return result, errors.New("stage fixture requires a worker-free, task-free idle graph")
	}
	residentCounts := make([]int, len(resources))
	seenResources := make(map[string]bool, len(resources))
	for _, contents := range snapshot.Pipeline.GetResources() {
		index := -1
		for i, resource := range resources {
			if resource.Name == contents.GetName() {
				index = i
				break
			}
		}
		if index < 0 || seenResources[contents.GetName()] {
			return result, fmt.Errorf("stage snapshot contains unexpected or duplicate resource %q", contents.GetName())
		}
		seenResources[contents.GetName()] = true
		count, err := stageResidentCount(contents, expected, valueWire)
		if err != nil {
			return result, err
		}
		residentCounts[index] = count
		result.RetainedEntries += count
	}
	if len(seenResources) != len(resources) {
		return result, errors.New("stage snapshot omitted requested resources")
	}
	result.RetainedStages = len(resources)
	if cfg.GraphOnly {
		return result, nil
	}
	build := func(n, ordinal int) (*bess.Transaction, error) {
		slot, gate := stageSelection(ordinal, cfg.Resident, cfg.Locality)
		tx := c.Transaction()
		for _, r := range resources[:n] {
			var addErr error
			if cfg.Mode == "encoded" {
				addErr = tx.UpsertEncoded(r, keyWire[slot], valueWire[gate])
			} else {
				addErr = tx.Upsert(r, keys[slot], values[gate])
			}
			if addErr != nil {
				return tx, addErr
			}
		}
		return tx, nil
	}
	seed := func(out *StageMeasurement, phase string) error {
		for ri, r := range resources {
			for start := 0; start < cfg.Resident; start += 512 {
				tx := c.Transaction()
				for i := start; i < min(start+512, cfg.Resident); i++ {
					if err := tx.UpsertEncoded(r, keyWire[i], valueWire[1]); err != nil {
						return err
					}
				}
				applied, err := stageCommit(ctx, tx, min(512, cfg.Resident-start), result.DaemonEpoch)
				receipt := stageOutcome(tx, 0, cfg.Resident, cfg.Locality, applied, err)
				receipt.Slot, receipt.Gate, receipt.Phase = start, 1, phase
				out.SeedReceipts = []StageOutcome{receipt}
				if err != nil {
					return fmt.Errorf("seeding %s: %w", r.Name, err)
				}
				if seeded := min(start+512, cfg.Resident); seeded > residentCounts[ri] {
					result.RetainedEntries += seeded - residentCounts[ri]
					residentCounts[ri] = seeded
				}
			}
		}
		return nil
	}
	for _, n := range cfg.Counts {
		row := StageMeasurement{Stages: n, OpsPerTransaction: n}
		result.Measurements = append(result.Measurements, row)
		out := &result.Measurements[len(result.Measurements)-1]
		// Restore the same seeded state in ALL stages, outside measurement.
		if err := seed(out, "prebuilt"); err != nil {
			return result, err
		}
		cold, err := build(n, 0)
		if err != nil {
			return result, err
		}
		out.ColdCommit, err = stageBlock(func() (int, error) {
			applied, commitErr := stageCommit(ctx, cold, n, result.DaemonEpoch)
			out.Cold = stageOutcome(cold, 0, cfg.Resident, cfg.Locality, applied, commitErr)
			return 1, commitErr
		})
		if err != nil {
			return result, err
		}
		out.KeyValueBytes = n * (len(keyWire[0]) + len(valueWire[2]))
		req := &pb.ApplyTransactionRequest{RequestId: cold.RequestID, DaemonEpoch: result.DaemonEpoch, Consistency: pb.ApplyTransactionRequest_CONSISTENCY_REFERENTIAL}
		for _, r := range resources[:n] {
			req.Ops = append(req.Ops, &pb.TransactionOp{Resource: r.Name,
				Key:   &anypb.Any{TypeUrl: "type.googleapis.com/" + r.KeyType, Value: keyWire[0]},
				Value: &anypb.Any{TypeUrl: "type.googleapis.com/" + r.ValueType, Value: valueWire[2]}})
		}
		out.RequestBytes = proto.Size(req)
		txs := make([]*bess.Transaction, cfg.Iterations)
		out.Build, err = stageBlock(func() (int, error) {
			for i := range txs {
				if ctx.Err() != nil {
					return i, ctx.Err()
				}
				txs[i], err = build(n, i+1)
				if err != nil {
					return i, err
				}
			}
			return len(txs), nil
		})
		if err != nil {
			return result, err
		}
		var ackSamples []time.Duration
		if cfg.MeasureAcks {
			ackSamples = make([]time.Duration, cfg.Iterations)
		}
		commit := func(ctx context.Context, tx *bess.Transaction, ordinal int) (*bess.Applied, error) {
			if ackSamples == nil {
				return stageCommit(ctx, tx, n, result.DaemonEpoch)
			}
			start := time.Now()
			applied, commitErr := stageCommit(ctx, tx, n, result.DaemonEpoch)
			ackSamples[ordinal-1] = time.Since(start)
			return applied, commitErr
		}
		if cfg.Concurrency == 1 {
			out.Commit, err = stageBlock(func() (int, error) {
				for i, tx := range txs {
					applied, commitErr := commit(ctx, tx, i+1)
					out.Last = stageOutcome(tx, i+1, cfg.Resident, cfg.Locality, applied, commitErr)
					txs[i] = nil
					if commitErr != nil {
						return i + 1, commitErr
					}
				}
				return len(txs), nil
			})
		} else {
			out.CommitWorkers = make([]StageWorkerOutcome, cfg.Concurrency)
			out.Commit, err = stageBlock(func() (int, error) {
				return stageWindow(ctx, cfg.Iterations, out.CommitWorkers,
					func(ctx context.Context, ordinal int) (StageOutcome, error) {
						tx := txs[ordinal-1]
						applied, commitErr := commit(ctx, tx, ordinal)
						outcome := stageOutcome(tx, ordinal, cfg.Resident, cfg.Locality, applied, commitErr)
						txs[ordinal-1] = nil
						return outcome, commitErr
					})
			})
			out.Last = stageLast(out.CommitWorkers)
		}
		if cfg.MeasureAcks {
			out.CommitAckLatency = stageAckSummary(ackSamples, n)
			clear(ackSamples)
		}
		if err != nil {
			return result, err
		}
		if err = seed(out, "combined"); err != nil {
			return result, err
		}
		cold, err = build(n, 0)
		if err != nil {
			return result, err
		}
		applied, err := stageCommit(ctx, cold, n, result.DaemonEpoch)
		out.CombinedCold = stageOutcome(cold, 0, cfg.Resident, cfg.Locality, applied, err)
		if err != nil {
			return result, err
		}
		if cfg.Concurrency == 1 {
			out.BuildCommit, err = stageBlock(func() (int, error) {
				for i := range cfg.Iterations {
					tx, buildErr := build(n, i+1)
					if buildErr != nil {
						return i, buildErr
					}
					applied, commitErr := commit(ctx, tx, i+1)
					out.CombinedLast = stageOutcome(tx, i+1, cfg.Resident, cfg.Locality, applied, commitErr)
					if commitErr != nil {
						return i + 1, commitErr
					}
				}
				return cfg.Iterations, nil
			})
		} else {
			out.CombinedWorkers = make([]StageWorkerOutcome, cfg.Concurrency)
			out.BuildCommit, err = stageBlock(func() (int, error) {
				return stageWindow(ctx, cfg.Iterations, out.CombinedWorkers,
					func(ctx context.Context, ordinal int) (StageOutcome, error) {
						tx, buildErr := build(n, ordinal)
						if buildErr != nil {
							return stageOutcome(tx, ordinal, cfg.Resident, cfg.Locality, nil, buildErr), buildErr
						}
						applied, commitErr := commit(ctx, tx, ordinal)
						return stageOutcome(tx, ordinal, cfg.Resident, cfg.Locality, applied, commitErr), commitErr
					})
			})
			out.CombinedLast = stageLast(out.CombinedWorkers)
		}
		if cfg.MeasureAcks {
			out.CombinedAckLatency = stageAckSummary(ackSamples, n)
		}
		if err != nil {
			return result, err
		}
	}
	return result, nil
}
