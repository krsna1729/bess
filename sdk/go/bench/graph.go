// SPDX-License-Identifier: BSD-3-Clause

package bench

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
	"google.golang.org/protobuf/types/known/anypb"

	bess "github.com/krsna1729/bess/sdk/go/bess"
	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// ApplyGraph applies the graph this package measures: Source -> RandomUpdate ->
// ExactMatch -> one Sink per gate, on one worker whose core is given. The
// returned function removes it again -- restoring whatever was applied before,
// if anything was and force allowed it -- and callers should defer it, including
// on interrupt. Nodes are named as the C++ live_transaction_bench names them, so
// the two measure the same shape under the same traffic.
//
// A daemon that already has a pipeline is left alone unless force is set: the
// floor is not worth someone's graph.
func ApplyGraph(ctx context.Context, c *bess.Client, cfg Config, workerCore int, force bool) (func(context.Context) error, error) {
	cfg = cfg.WithDefaults()
	snap, err := c.Pipeline(ctx)
	if err != nil {
		return nil, fmt.Errorf("reading the pipeline: %w", err)
	}
	previous := snap.Pipeline
	if n := len(previous.GetModules()); n > 0 && !force {
		return nil, fmt.Errorf("the daemon already runs a pipeline (%d modules); pass -force to replace it for the run and restore it after", n)
	}
	sc, err := c.Schema(ctx)
	if err != nil {
		return nil, fmt.Errorf("schema: %w", err)
	}
	files, err := protodesc.NewFiles(sc.GetDescriptors())
	if err != nil {
		return nil, fmt.Errorf("the daemon's descriptors: %w", err)
	}
	classes := make(map[string]protoreflect.MessageDescriptor, len(sc.GetModuleClasses()))
	for _, mc := range sc.GetModuleClasses() {
		if md, err := message(files, protoreflect.FullName(mc.GetInitArgType())); err == nil {
			classes[mc.GetName()] = md
		}
	}
	arg := func(class string, fields map[string]any) (*anypb.Any, error) {
		md, ok := classes[class]
		if !ok {
			return nil, fmt.Errorf("this daemon has no module class %q to build the bench graph from", class)
		}
		m := dynamicpb.NewMessage(md)
		if fields != nil {
			raw, err := json.Marshal(fields)
			if err != nil {
				return nil, fmt.Errorf("%s init: %w", class, err)
			}
			if err := protojson.Unmarshal(raw, m); err != nil {
				return nil, fmt.Errorf("%s init from %s: %w", class, raw, err)
			}
		}
		return anypb.New(m)
	}

	pipeline := &pb.Pipeline{
		Modules: []*pb.Module{},
		Workers: []*pb.Worker{{Wid: 0, Core: proto.Int32(int32(workerCore))}},
	}
	sourceArg, err := arg("Source", map[string]any{"pkt_size": cfg.PktSize})
	if err != nil {
		return nil, err
	}
	updateArg, err := arg("RandomUpdate", map[string]any{"fields": []any{map[string]any{
		"offset": 26, "size": 4, "min": cfg.Base, "max": cfg.Base + LiveAddrs - 1}}})
	if err != nil {
		return nil, err
	}
	matchArg, err := arg("ExactMatch", map[string]any{"fields": []any{map[string]any{
		"offset": 26, "num_bytes": 4}}})
	if err != nil {
		return nil, err
	}
	pipeline.Modules = append(pipeline.Modules,
		&pb.Module{Name: "src", Mclass: "Source", Arg: sourceArg},
		&pb.Module{Name: "rnd", Mclass: "RandomUpdate", Arg: updateArg},
		&pb.Module{Name: MatchModule, Mclass: "ExactMatch", Arg: matchArg},
	)
	for gate := 0; gate <= cfg.Gates; gate++ {
		pipeline.Modules = append(pipeline.Modules, &pb.Module{
			Name: fmt.Sprintf("sink%d", gate), Mclass: "Sink"})
	}
	chain := func(up, down string) {
		pipeline.Connections = append(pipeline.Connections,
			&pb.Connection{Upstream: up, Ogate: 0, Downstream: down, Igate: 0})
	}
	chain("src", "rnd")
	chain("rnd", MatchModule)
	for gate := 0; gate <= cfg.Gates; gate++ {
		pipeline.Connections = append(pipeline.Connections, &pb.Connection{
			Upstream: MatchModule, Ogate: uint32(gate),
			Downstream: fmt.Sprintf("sink%d", gate), Igate: 0})
	}

	if err := c.PauseWorkers(ctx); err != nil {
		return nil, fmt.Errorf("pausing workers: %w", err)
	}
	if _, err := c.ApplyPipeline(ctx, pipeline, bess.WithExpectedVersion(snap.Version)); err != nil {
		_ = c.ResumeWorkers(ctx)
		return nil, fmt.Errorf("applying the bench graph: %w", err)
	}
	if err := c.ResumeWorkers(ctx); err != nil {
		return nil, fmt.Errorf("resuming workers: %w", err)
	}

	return func(cctx context.Context) error {
		_ = c.PauseWorkers(cctx)
		defer c.ResumeWorkers(cctx)
		restore := previous
		if restore == nil || len(restore.GetModules()) == 0 {
			restore = &pb.Pipeline{}
		}
		cur, err := c.Pipeline(cctx)
		if err != nil {
			return err
		}
		if _, err := c.ApplyPipeline(cctx, restore, bess.WithExpectedVersion(cur.Version)); err != nil {
			return fmt.Errorf("removing the bench graph: %w", err)
		}
		return nil
	}, nil
}

// MatchModule is the name the bench graph gives its ExactMatch module, and so
// the name an importer's own graph needs if they would rather keep theirs.
const MatchModule = "em"

// WaitSettled gives the workers a moment to pick up a newly applied graph. It is
// what the C++ bench sleeps for, kept here so both wait the same way.
const WaitSettled = time.Second
