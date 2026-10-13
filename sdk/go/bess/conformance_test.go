// SPDX-License-Identifier: BSD-3-Clause

//go:build live

package bess

// The Go SDK's driver of the control SDK conformance suite (D-104): runs
// sdk/conformance/cases.json against a live bessd, the steps as defined in
// docs/control-sdk.md ("Conformance suite"), one subtest per case.
// sdk/cpp/conformance.cc is the C++ one; tests/live/conformance_test.cc runs
// both; it writes testdata/conformance.json (the daemon's address and the
// resource), a file so that a Go run in a container sees it too.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"os"
	"reflect"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	"google.golang.org/protobuf/types/dynamicpb"
	"google.golang.org/protobuf/types/known/anypb"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

// spec is one object of the cases file: a case, a step or an op.
type spec map[string]any

func (s spec) str(name string) string   { v, _ := s[name].(string); return v }
func (s spec) boolean(name string) bool { v, _ := s[name].(bool); return v }
func (s spec) has(name string) bool     { _, ok := s[name]; return ok }
func (s spec) num(name string) float64  { v, _ := s[name].(float64); return v }
func (s spec) list(name string) []any   { v, _ := s[name].([]any); return v }

// kindName names an SDK error as the cases do.
func kindName(err error) string {
	var (
		invalid   *InvalidRequestError
		conflict  *ConflictError
		rejected  *RejectedError
		busy      *BusyError
		transport *TransportError
		pconflict *PipelineConflictError
		restarted *DaemonRestartedError
		stale     *StaleResourceError
	)
	switch {
	case errors.As(err, &invalid):
		return "invalid_request"
	case errors.As(err, &conflict):
		return "conflict"
	case errors.As(err, &rejected):
		return "rejected"
	case errors.As(err, &busy):
		return "busy"
	case errors.As(err, &transport):
		return "transport"
	case errors.As(err, &pconflict):
		return "pipeline_conflict"
	case errors.As(err, &restarted):
		return "daemon_restarted"
	case errors.As(err, &stale):
		return "stale_resource"
	case errors.Is(err, context.Canceled):
		return "cancelled"
	}
	return "unknown"
}

// expect: whether a call that returned err is what the step wants (want: an
// error kind, or "" for success).
func expect(err error, want, what string) error {
	switch {
	case err == nil && want == "":
		return nil
	case err == nil:
		return fmt.Errorf("%s: succeeded, want %s", what, want)
	case want == "" || kindName(err) != want:
		suffix := ""
		if want != "" {
			suffix = ", want " + want
		}
		return fmt.Errorf("%s: %s (%v)%s", what, kindName(err), err, suffix)
	}
	return nil
}

type commitLabel struct {
	requestID  string
	generation uint64
}

type pipeLabel struct {
	pipeline *pb.Pipeline
	version  PipelineVersion
}

type watch struct {
	mu     sync.Mutex
	events []*pb.Event
	stop   context.CancelFunc
}

type runner struct {
	ctx      context.Context
	client   *Client
	resource string
	schema   *pb.GetSchemaResponse
	files    *protoregistry.Files
	commits  map[string]commitLabel
	pipes    map[string]pipeLabel
	watches  map[string]*watch
}

func (r *runner) message(typ string, value any) (proto.Message, error) {
	d, err := r.files.FindDescriptorByName(protoreflect.FullName(typ))
	if err != nil {
		return nil, fmt.Errorf("no type %s in the schema", typ)
	}
	text, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}
	m := dynamicpb.NewMessage(d.(protoreflect.MessageDescriptor))
	if err := protojson.Unmarshal(text, m); err != nil {
		return nil, fmt.Errorf("%s from %s: %v", typ, text, err)
	}
	return m, nil
}

func (r *runner) runCase(c spec) error {
	r.commits = map[string]commitLabel{}
	r.pipes = map[string]pipeLabel{}
	r.watches = map[string]*watch{}
	defer func() {
		for _, w := range r.watches {
			w.stop()
		}
	}()
	for i, s := range c.list("steps") {
		st := spec(s.(map[string]any))
		if err := r.step(st); err != nil {
			return fmt.Errorf("step %d (%s): %v", i, st.str("do"), err)
		}
	}
	return nil
}

func (r *runner) step(s spec) error {
	wantError := s.str("want_error")
	switch what := s.str("do"); what {
	case "capabilities":
		caps, err := r.client.Capabilities(r.ctx)
		if err != nil {
			return expect(err, wantError, what)
		}
		if caps.GetDaemonVersion() == "" || len(caps.GetModuleClasses()) == 0 {
			return errors.New("no daemon version or module classes")
		}
		if caps.GetPluginApiVersion() == 0 || caps.GetDaemonEpoch() == 0 {
			return errors.New("no plugin API version or daemon epoch")
		}
		var resources []string
		for _, res := range caps.GetResources() {
			resources = append(resources, res.GetName())
		}
		for field, have := range map[string][]string{
			"want_rpcs": caps.GetRpcs(), "want_classes": caps.GetModuleClasses(), "want_resources": resources,
		} {
			for _, want := range s.list(field) {
				name := r.vary(want.(string))
				if !slices.Contains(have, name) {
					return fmt.Errorf("%s: no %s", field, name)
				}
			}
		}
	case "supports":
		ok, err := r.client.Supports(r.ctx, s.str("rpc"))
		if err != nil {
			return expect(err, wantError, what)
		}
		if ok != s.boolean("want") {
			return fmt.Errorf("Supports(%s) = %v", s.str("rpc"), ok)
		}
	case "metrics":
		samples, err := r.client.Metrics(r.ctx)
		if err != nil {
			return expect(err, wantError, what)
		}
		if len(samples) == 0 {
			return errors.New("no samples")
		}
	case "schema":
		for _, c := range r.schema.GetModuleClasses() {
			if c.GetName() == s.str("class") {
				if _, err := r.files.FindDescriptorByName(protoreflect.FullName(c.GetInitArgType())); err != nil {
					return fmt.Errorf("init type '%s' not in the descriptors", c.GetInitArgType())
				}
				return nil
			}
		}
		return errors.New("no class " + s.str("class"))
	case "resource":
		name := r.vary(s.str("name"))
		_, err := r.client.Resource(r.ctx, name)
		return expect(err, wantError, what)
	case "watch":
		return r.openWatch(s)
	case "event":
		return r.awaitEvent(s)
	case "commit":
		return r.commit(s, wantError)
	case "transaction_status":
		id := s.str("request_id")
		if s.has("of") {
			id = r.commits[s.str("of")].requestID
		}
		known, rec, _, err := r.client.GetTransaction(r.ctx, id)
		if err != nil {
			return expect(err, wantError, what)
		}
		if known != s.boolean("want_known") {
			return fmt.Errorf("known = %v", known)
		}
		if s.has("want_outcome") && rec.GetOutcome().String() != s.str("want_outcome") {
			return fmt.Errorf("outcome %s", rec.GetOutcome())
		}
	case "pipeline":
		return r.pipeline(s, wantError)
	case "action":
		return r.action(s, wantError)
	default:
		return fmt.Errorf("unknown step '%s'", what)
	}
	return nil
}

// vary substitutes the case variables: $resource, and $module (the
// resource's module) alone or before "/<table>".
func (r *runner) vary(s string) string {
	if s == "$resource" {
		return r.resource
	}
	if rest, ok := strings.CutPrefix(s, "$module"); ok && (rest == "" || strings.HasPrefix(rest, "/")) {
		module, _, _ := strings.Cut(r.resource, "/")
		return module + rest
	}
	return s
}

// action: the argument's type is arg_type, else the action's in class's
// schema.
func (r *runner) action(s spec, wantError string) error {
	action := s.str("action")
	typ := s.str("arg_type")
	for _, c := range r.schema.GetModuleClasses() {
		for _, a := range c.GetActions() {
			if typ == "" && c.GetName() == s.str("class") && a.GetName() == action {
				typ = a.GetArgType()
			}
		}
	}
	if typ == "" {
		return fmt.Errorf("%s has no action %s in the schema", s.str("class"), action)
	}
	arg, err := r.message(typ, s["arg"])
	if err != nil {
		return err
	}
	result, err := r.client.InvokeAction(r.ctx, r.vary(s.str("module")), action, arg)
	if err := expect(err, wantError, action); err != nil || wantError != "" {
		return err
	}
	want, ok := s["want_answer"].(map[string]any)
	if !ok {
		return nil
	}
	got, err := anypb.UnmarshalNew(result, proto.UnmarshalOptions{Resolver: dynamicpb.NewTypes(r.files)})
	if err != nil {
		return fmt.Errorf("answer %s: %v", result.GetTypeUrl(), err)
	}
	text, err := protojson.MarshalOptions{UseProtoNames: true}.Marshal(got)
	if err != nil {
		return err
	}
	var fields map[string]any
	if err := json.Unmarshal(text, &fields); err != nil {
		return err
	}
	for k, v := range want {
		if !reflect.DeepEqual(fields[k], v) {
			return fmt.Errorf("answer %s: want %s = %v", text, k, v)
		}
	}
	return nil
}

// resourceTypes is the schema of resource name ("<module>/<table>"): the
// table of the class of its module in p.
func (r *runner) resourceTypes(p *pb.Pipeline, name string) (*pb.ResourceSchema, error) {
	module, table, _ := strings.Cut(name, "/")
	for _, m := range p.GetModules() {
		if m.GetName() != module {
			continue
		}
		for _, c := range r.schema.GetModuleClasses() {
			if c.GetName() != m.GetMclass() {
				continue
			}
			for _, t := range c.GetResources() {
				if t.GetTable() == table {
					return t, nil
				}
			}
		}
	}
	return nil, fmt.Errorf("no resource %s in the schemas of the pipeline's modules", name)
}

// entry builds one resource entry of the cases ({"key": ..., "value": ...};
// a singleton's has no key, and key is then nil) in t's types.
func (r *runner) entry(t *pb.ResourceSchema, e any) (key, value proto.Message, err error) {
	es, _ := e.(map[string]any)
	if t.GetKind() != pb.TransactionResource_KIND_SINGLETON {
		if _, ok := es["key"]; !ok {
			return nil, nil, errors.New("no key")
		}
		if key, err = r.message(t.GetKeyType(), es["key"]); err != nil {
			return nil, nil, err
		}
	}
	if _, ok := es["value"]; !ok {
		return nil, nil, errors.New("no value")
	}
	value, err = r.message(t.GetValueType(), es["value"])
	return key, value, err
}

// listResources lists each resource of listed in p with exactly its entries
// (in place of any listing p had): a map's entries, a singleton's value.
func (r *runner) listResources(p *pb.Pipeline, listed map[string]any) error {
	for _, n := range slices.Sorted(maps.Keys(listed)) {
		name := r.vary(n)
		t, err := r.resourceTypes(p, name)
		if err != nil {
			return err
		}
		contents := &pb.ResourceContents{Name: name}
		entries, _ := listed[n].([]any)
		for i, e := range entries {
			key, value, err := r.entry(t, e)
			if err != nil {
				return fmt.Errorf("%s entry %d: %v", name, i, err)
			}
			entry := &pb.ResourceEntry{}
			if entry.Value, err = anypb.New(value); err != nil {
				return err
			}
			if key == nil { // a singleton has one value
				contents.Entries = []*pb.ResourceEntry{entry}
				continue
			}
			if entry.Key, err = anypb.New(key); err != nil {
				return err
			}
			contents.Entries = append(contents.Entries, entry)
		}
		p.Resources = slices.DeleteFunc(p.Resources, func(c *pb.ResourceContents) bool { return c.GetName() == name })
		p.Resources = append(p.Resources, contents)
	}
	return nil
}

// same: whether a holds m.
func same(m proto.Message, a *anypb.Any) bool {
	got := m.ProtoReflect().New().Interface()
	return proto.Unmarshal(a.GetValue(), got) == nil && proto.Equal(got, m)
}

// checkResources: p lists each resource of want with exactly its entries, in
// any order, compared as messages.
func (r *runner) checkResources(p *pb.Pipeline, want map[string]any) error {
	for _, n := range slices.Sorted(maps.Keys(want)) {
		name := r.vary(n)
		t, err := r.resourceTypes(p, name)
		if err != nil {
			return err
		}
		i := slices.IndexFunc(p.GetResources(), func(c *pb.ResourceContents) bool { return c.GetName() == name })
		if i < 0 {
			return fmt.Errorf("%s was not read", name)
		}
		have := p.GetResources()[i].GetEntries()
		entries, _ := want[n].([]any)
		if len(have) != len(entries) {
			return fmt.Errorf("%s has %d entries, want %d", name, len(have), len(entries))
		}
		used := make([]bool, len(have))
		for j, e := range entries {
			key, value, err := r.entry(t, e)
			if err != nil {
				return fmt.Errorf("%s entry %d: %v", name, j, err)
			}
			found := false
			for k, h := range have {
				if !used[k] && same(value, h.GetValue()) && (key == nil || same(key, h.GetKey())) {
					used[k], found = true, true
					break
				}
			}
			if !found {
				return fmt.Errorf("%s has no entry %d as wanted", name, j)
			}
		}
	}
	return nil
}

// sameVersion compares versions by value (ResourceGeneration is a pointer).
func sameVersion(a, b PipelineVersion) bool {
	if a.Generation != b.Generation || a.DaemonEpoch != b.DaemonEpoch ||
		(a.ResourceGeneration == nil) != (b.ResourceGeneration == nil) {
		return false
	}
	return a.ResourceGeneration == nil || *a.ResourceGeneration == *b.ResourceGeneration
}

func (r *runner) openWatch(s spec) error {
	var types []string
	for _, t := range s.list("types") {
		types = append(types, t.(string))
	}
	ctx, cancel := context.WithCancel(r.ctx)
	w := &watch{stop: cancel}
	// From sequence 1: what the daemon holds, then live; events are matched
	// by request id, so no race with what the case does next.
	events, _ := r.client.WatchEvents(ctx, 1, 0, types...)
	go func() {
		for e := range events {
			w.mu.Lock()
			w.events = append(w.events, e)
			w.mu.Unlock()
		}
	}()
	r.watches[s.str("as")] = w
	return nil
}

func (r *runner) awaitEvent(s spec) error {
	w, ok := r.watches[s.str("watch")]
	if !ok {
		return errors.New("no watch " + s.str("watch"))
	}
	of := r.commits[s.str("of")]
	var found *pb.Event
	for deadline := time.Now().Add(5 * time.Second); found == nil && time.Now().Before(deadline); {
		w.mu.Lock()
		for _, e := range w.events {
			if e.GetFields()["request_id"] == of.requestID {
				found = e
				break
			}
		}
		w.mu.Unlock()
		if found == nil {
			time.Sleep(20 * time.Millisecond)
		}
	}
	if found == nil {
		return errors.New("no event for " + of.requestID)
	}
	if found.GetGeneration() != of.generation {
		return fmt.Errorf("event generation %d, commit's %d", found.GetGeneration(), of.generation)
	}
	if fields, ok := s["want_fields"].(map[string]any); ok {
		for k, v := range fields {
			if found.GetFields()[k] != v.(string) {
				return fmt.Errorf("event %v: want %s=%s", found, k, v)
			}
		}
	}
	return nil
}

func (r *runner) commit(s spec, wantError string) error {
	res, err := r.client.Resource(r.ctx, r.resource)
	if err != nil {
		return fmt.Errorf("resource: %v", err)
	}
	var opts []TxOption
	if s.has("request_id_of") {
		opts = append(opts, WithRequestID(r.commits[s.str("request_id_of")].requestID))
	}
	if s.has("expected_generation_of") {
		g := int64(r.commits[s.str("expected_generation_of")].generation) + int64(s.num("generation_delta"))
		opts = append(opts, WithExpectedGeneration(uint64(g)))
	}
	if s.boolean("snapshot") {
		opts = append(opts, WithSnapshot())
	}
	tx := r.client.Transaction(opts...)
	for i, o := range s.list("ops") {
		op := spec(o.(map[string]any))
		keyType := res.KeyType
		if op.boolean("key_as_value") {
			keyType = res.ValueType
		}
		key, err := r.message(keyType, op["key"])
		if err != nil {
			return fmt.Errorf("op %d: %v", i, err)
		}
		var added error
		if op.str("op") == "upsert" {
			value, err := r.message(res.ValueType, op["value"])
			if err != nil {
				return fmt.Errorf("op %d: %v", i, err)
			}
			added = tx.Upsert(res, key, value)
		} else {
			added = tx.Erase(res, key)
		}
		if err := expect(added, op.str("want_error"), fmt.Sprintf("op %d", i)); err != nil {
			return err
		}
	}
	applied, err := tx.Commit(r.ctx)
	if err := expect(err, wantError, "commit"); err != nil {
		return err
	}
	if err != nil {
		if s.has("want_failed_ops") {
			var rejected *RejectedError
			if !errors.As(err, &rejected) {
				return fmt.Errorf("want_failed_ops needs a rejected error, got %v", err)
			}
			var failed, want []int
			for _, f := range rejected.Failures {
				failed = append(failed, f.Index)
			}
			for _, i := range s.list("want_failed_ops") {
				want = append(want, int(i.(float64)))
			}
			if !slices.Equal(failed, want) {
				return fmt.Errorf("failed operations differ: %v", err)
			}
		}
		return nil
	}
	if s.has("want_replayed") && applied.Replayed != s.boolean("want_replayed") {
		return fmt.Errorf("replayed = %v", applied.Replayed)
	}
	if s.has("want_generation_of") && applied.Generation != r.commits[s.str("want_generation_of")].generation {
		return fmt.Errorf("generation %d", applied.Generation)
	}
	if s.has("want_visibility") && applied.Visibility.String() != s.str("want_visibility") {
		return fmt.Errorf("visibility %s", applied.Visibility)
	}
	if s.has("as") {
		r.commits[s.str("as")] = commitLabel{applied.RequestID, applied.Generation}
	}
	return nil
}

func (r *runner) pipeline(s spec, wantError string) error {
	// base alone builds too: that pipeline applied again.
	builds := s.has("add") || s.has("chain") || s.has("remove") || s.has("base") || s.has("resources")
	var base *pb.Pipeline
	if s.has("base") {
		base = r.pipes[s.str("base")].pipeline
	} else {
		var query []PipelineQuery
		if s.has("with_resources") {
			var names []string
			for _, n := range s.list("with_resources") {
				names = append(names, r.vary(n.(string)))
			}
			query = append(query, WithResources(names...))
		}
		snap, err := r.client.Pipeline(r.ctx, query...)
		if err != nil {
			return expect(err, wantError, "pipeline")
		}
		if want, ok := s["want_resources"].(map[string]any); ok {
			if err := r.checkResources(snap.Pipeline, want); err != nil {
				return err
			}
		}
		if !builds {
			r.pipes[s.str("as")] = pipeLabel{snap.Pipeline, snap.Version}
			return nil
		}
		base = snap.Pipeline
	}
	b := NewPipeline(base)
	for _, m := range s.list("add") {
		add := m.([]any)
		name, class := add[0].(string), add[1].(string)
		var arg proto.Message
		if len(add) > 2 { // in the class's init type
			typ := ""
			for _, c := range r.schema.GetModuleClasses() {
				if c.GetName() == class {
					typ = c.GetInitArgType()
				}
			}
			var err error
			if arg, err = r.message(typ, add[2]); err != nil {
				return fmt.Errorf("add %s: %v", name, err)
			}
		}
		b.Module(name, class, arg)
	}
	var chain []string
	for _, n := range s.list("chain") {
		chain = append(chain, n.(string))
	}
	if len(chain) > 0 {
		b.Chain(chain...)
	}
	removed := map[string]bool{}
	for _, n := range s.list("remove") {
		b.Remove(n.(string))
		removed[n.(string)] = true
	}
	p, err := b.Build()
	if err != nil {
		return expect(err, wantError, "build")
	}
	// A removed module's resources ("<module>/...") go with it, as the C++
	// builder's Remove drops them.
	p.Resources = slices.DeleteFunc(p.Resources, func(c *pb.ResourceContents) bool {
		module, _, _ := strings.Cut(c.GetName(), "/")
		return removed[module]
	})
	if listed, ok := s["resources"].(map[string]any); ok {
		if err := r.listResources(p, listed); err != nil {
			return err
		}
	}
	if s.boolean("plan") {
		steps, v, err := r.client.PlanPipeline(r.ctx, p)
		if err != nil || len(steps) == 0 {
			return fmt.Errorf("plan: %d step(s), %v", len(steps), err)
		}
		if s.has("want_plan_version_of") && !sameVersion(v, r.pipes[s.str("want_plan_version_of")].version) {
			return fmt.Errorf("plan against generation %d of epoch %d", v.Generation, v.DaemonEpoch)
		}
	}
	if s.has("want_changes") {
		diff, _, err := r.client.DiffPipeline(r.ctx, p)
		if err != nil {
			return fmt.Errorf("diff: %v", err)
		}
		if len(diff.GetChanges()) != int(s.num("want_changes")) {
			return fmt.Errorf("%d change(s)", len(diff.GetChanges()))
		}
	}
	var opts []PipelineOption
	if s.has("at_version_of") {
		opts = append(opts, WithExpectedVersion(r.pipes[s.str("at_version_of")].version))
	}
	done, err := r.client.ApplyPipeline(r.ctx, p, opts...)
	if err := expect(err, wantError, "apply"); err != nil || wantError != "" {
		return err
	}
	if s.has("want_generation_of") && done.GetGeneration() != r.pipes[s.str("want_generation_of")].version.Generation {
		return fmt.Errorf("applied at generation %d, want %s's", done.GetGeneration(), s.str("want_generation_of"))
	}
	if s.has("want_resource_ops") && done.GetResourceOps() != uint64(s.num("want_resource_ops")) {
		return fmt.Errorf("%d resource op(s)", done.GetResourceOps())
	}
	if s.has("want_resource_generation_of") {
		want := r.pipes[s.str("want_resource_generation_of")].version.ResourceGeneration
		if want == nil || done.GetResourceGeneration() != *want {
			return fmt.Errorf("applied at resource generation %d, want %s's", done.GetResourceGeneration(),
				s.str("want_resource_generation_of"))
		}
	}
	if s.has("as") {
		v := PipelineVersion{Generation: done.GetGeneration(), DaemonEpoch: done.GetDaemonEpoch()}
		if len(p.GetResources()) > 0 {
			v.ResourceGeneration = proto.Uint64(done.GetResourceGeneration())
		}
		r.pipes[s.str("as")] = pipeLabel{p, v}
	}
	return nil
}

func TestConformance(t *testing.T) {
	raw, err := os.ReadFile("testdata/conformance.json")
	if err != nil {
		t.Skip("no testdata/conformance.json (tests/live/conformance_test.cc writes it)")
	}
	var cfg struct {
		Address  string `json:"address"`
		Resource string `json:"resource"`
	}
	if err := json.Unmarshal(raw, &cfg); err != nil {
		t.Fatal(err)
	}
	address, resource := cfg.Address, cfg.Resource
	if raw, err = os.ReadFile("../../conformance/cases.json"); err != nil {
		t.Fatal(err)
	}
	var cases struct {
		Cases []map[string]any `json:"cases"`
	}
	if err := json.Unmarshal(raw, &cases); err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	client, err := Dial(address)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	schema, err := client.Schema(ctx)
	if err != nil {
		t.Fatal(err)
	}
	files, err := protodesc.NewFiles(schema.GetDescriptors())
	if err != nil {
		t.Fatal(err)
	}
	r := &runner{ctx: ctx, client: client, resource: resource, schema: schema, files: files}
	for _, c := range cases.Cases {
		c := spec(c)
		t.Run(c.str("name"), func(t *testing.T) {
			if err := r.runCase(c); err != nil {
				t.Fatal(err)
			}
		})
	}
}
