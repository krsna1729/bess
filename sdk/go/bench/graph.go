// SPDX-License-Identifier: BSD-3-Clause

package bench

import (
	"context"
	"encoding/json"
	"errors"
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
// returned function removes it again -- putting back the pipeline that was
// applied before, with the resource contents the daemon reported, if anything
// was and force allowed it -- and callers should defer it, including on
// interrupt. Nodes are named as the C++ live_transaction_bench names them, so
// the two measure the same shape under the same traffic.
//
// A daemon that already has a pipeline is left alone unless force is set: the
// floor is not worth someone's graph. With force, what comes back is the
// pipeline as it was read -- its modules, its settings, and the contents of the
// resources it reported -- so anything a snapshot does not carry does not come
// back. A daemon that is serving traffic is still best left alone: two runs
// against an idle one measure the same thing.
func ApplyGraph(ctx context.Context, c *bess.Client, cfg Config, workerCore int, force bool) (func(context.Context) error, error) {
	cfg = cfg.WithDefaults()
	// WithResources with no names means every resource: the snapshot has to
	// carry their contents, or a restore would put the daemon's modules back
	// with their tables emptied, which is worse than not offering to restore.
	snap, err := c.Pipeline(ctx, bess.WithResources())
	if err != nil {
		return nil, fmt.Errorf("reading the pipeline: %w", err)
	}
	previous := snap.Pipeline
	if n := len(previous.GetModules()); n > 0 && !force {
		return nil, fmt.Errorf("the daemon already runs a pipeline (%d modules, %d resources); pass -force to replace it for the run and restore it after",
			n, len(previous.GetResources()))
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
		// A class whose init message the schema does not carry cannot be built;
		// arg() reports that by name if the graph asks for it.
		md, merr := message(files, protoreflect.FullName(mc.GetInitArgType()))
		if merr == nil {
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
			raw, merr := json.Marshal(fields)
			if merr != nil {
				return nil, fmt.Errorf("%s init: %w", class, merr)
			}
			if uerr := protojson.Unmarshal(raw, m); uerr != nil {
				return nil, fmt.Errorf("%s init from %s: %w", class, raw, uerr)
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

	remove := func(cctx context.Context) error {
		return paused(cctx, c.PauseWorkers, c.ResumeWorkers, func() error {
			// The bench graph comes off first, for the same reason the caller's
			// had to: its modules carry the names the restored ones carry.
			cur, err := c.Pipeline(cctx)
			if err != nil {
				return err
			}
			if _, err = c.ApplyPipeline(cctx, &pb.Pipeline{}, bess.WithExpectedVersion(cur.Version)); err != nil {
				return fmt.Errorf("removing the bench graph: %w", err)
			}
			back := previous
			if back == nil || len(back.GetModules()) == 0 {
				return nil
			}
			cur, err = c.Pipeline(cctx)
			if err != nil {
				return err
			}
			if _, err = c.ApplyPipeline(cctx, back, bess.WithExpectedVersion(cur.Version)); err != nil {
				return fmt.Errorf("putting the daemon's own pipeline back: %w", err)
			}
			return nil
		})
	}
	if err := c.PauseWorkers(ctx); err != nil {
		return nil, fmt.Errorf("pausing workers: %w", err)
	}
	// A module that is already there with a different argument cannot be
	// replaced in the same transaction that renames it: with force, the old
	// graph comes off first and only then does the bench graph go on.
	version := snap.Version
	if len(previous.GetModules()) > 0 {
		if _, err := c.ApplyPipeline(ctx, &pb.Pipeline{}, bess.WithExpectedVersion(version)); err != nil {
			return nil, resumeAfterFailure(ctx, c, fmt.Errorf("clearing the daemon's pipeline: %w", err))
		}
		cur, err := c.Pipeline(ctx)
		if err != nil {
			return nil, resumeAfterFailure(ctx, c, fmt.Errorf("reading the pipeline after clearing it: %w", err))
		}
		version = cur.Version
	}
	if _, err := c.ApplyPipeline(ctx, pipeline, bess.WithExpectedVersion(version)); err != nil {
		err = resumeAfterFailure(ctx, c, err)
		// Force cleared the daemon's own pipeline, so a failure here leaves it
		// with nothing at all: put back what was there before reporting it.
		if len(previous.GetModules()) > 0 {
			if rerr := remove(ctx); rerr != nil {
				return remove, fmt.Errorf("applying the bench graph: %w (and putting the daemon's pipeline back failed too: %v)", err, rerr)
			}
			// The daemon's own pipeline is back, so there is nothing left to
			// clean up: handing back the cleanup would run it a second time.
			return nil, fmt.Errorf("applying the bench graph: %w", err)
		}
		return remove, fmt.Errorf("applying the bench graph: %w", err)
	}
	if err := c.ResumeWorkers(ctx); err != nil {
		// The graph is on the daemon; hand back what takes it off anyway.
		return remove, fmt.Errorf("resuming workers: %w", err)
	}
	return remove, nil
}

// paused runs body with the daemon's workers paused. A resume that fails is
// reported, because it leaves the daemon paused. A pause that fails is reported
// too, but does not stop body: ApplyPipeline is safe with the workers running,
// and leaving the daemon holding the bench graph, with the caller's own pipeline
// gone, would be the worse outcome.
func paused(ctx context.Context, pause, resume func(context.Context) error, body func() error) (err error) {
	defer func() {
		if rerr := resume(ctx); rerr != nil {
			err = errors.Join(err, fmt.Errorf("resuming the workers: %w", rerr))
		}
	}()
	pauseErr := pause(ctx)
	err = body()
	if pauseErr != nil {
		err = errors.Join(fmt.Errorf("pausing the workers: %w", pauseErr), err)
	}
	return err
}

// resumeAfterFailure resumes the workers on an error path and returns err with
// the resume's own failure folded in. The caller is already reporting err, and a
// resume that also fails would otherwise leave the daemon paused silently.
func resumeAfterFailure(ctx context.Context, c *bess.Client, err error) error {
	if rerr := c.ResumeWorkers(ctx); rerr != nil {
		return fmt.Errorf("%w (and resuming the workers failed too: %v)", err, rerr)
	}
	return err
}

// MatchModule is the name the bench graph gives its ExactMatch module, and so
// the name an importer's own graph needs if they would rather keep theirs.
const MatchModule = "em"

// WaitSettled gives the workers a moment to pick up a newly applied graph. It is
// what the C++ bench sleeps for, kept here so both wait the same way.
const WaitSettled = time.Second
