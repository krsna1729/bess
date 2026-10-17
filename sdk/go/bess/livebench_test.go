// SPDX-License-Identifier: BSD-3-Clause

// Benchmarks against a live bessd, skipped unless BESS_BENCH_URL names one.
//
// The benchmarks in bench_test.go measure this package against a stub daemon
// over bufconn, which is the client's own cost. These measure the same package
// against a real daemon, on the shape the C++ live_transaction_bench drives: k
// upserts and k erases per transaction on one ExactMatch module's `em/rules`.
// Equal counts keep the table's size steady, so latency is a fixed cost plus a
// cost per operation and not table growth. Commit alone is timed, as the C++
// bench times it, and the report is per operation, so the two are comparable.
//
// A pipeline with an ExactMatch module named em must be applied first;
// live_transaction_bench --keep leaves its own in place, traffic included.
//
//	BESS_BENCH_URL=localhost:10514 go test -run='^$' -bench=Live -benchtime=2s ./bess/
//
// The client's retry policy is not settable here, so a busy answer that the
// daemon passes on costs the default 10 ms before the retry; the busy count in
// the report says whether that happened.
package bess

import (
	"context"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"os"
	"sort"
	"testing"
	"time"

	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	"google.golang.org/protobuf/types/dynamicpb"
)

// liveAddrs is how many addresses cycle and livePreload how many rules are
// loaded before the first measured transaction, so that the keys a transaction
// erases are never the ones it inserts; liveAddrs must exceed livePreload plus
// the largest transaction. livePreload stays under the 1000 operations the C++
// bench loads per transaction. liveGates matches the four gates that bench
// wires beyond the default, and liveBase keeps these addresses out of its live
// set, which a --keep run leaves in the table.
const (
	liveAddrs   = 4096
	livePreload = 1000
	liveBase    = 0x0b000000
	liveGates   = 4
)

// liveShape is a client bound to a running daemon's rule table, with one
// prebuilt message per address and gate: the messages are read-only, so the
// loop pays nothing for building them.
type liveShape struct {
	client *Client
	rules  Resource
	keys   []proto.Message
	values []proto.Message
}

func newLiveShape(b *testing.B) *liveShape {
	b.Helper()
	url := os.Getenv("BESS_BENCH_URL")
	if url == "" {
		b.Skip("BESS_BENCH_URL is unset: these benchmarks need a running bessd with em/rules applied")
	}
	ctx := context.Background()
	c, err := Dial(url)
	if err != nil {
		b.Fatalf("dial %s: %v", url, err)
	}
	b.Cleanup(func() { _ = c.Close() })
	sc, err := c.Schema(ctx)
	if err != nil {
		b.Fatalf("schema: %v", err)
	}
	files, err := protodesc.NewFiles(sc.GetDescriptors())
	if err != nil {
		b.Fatalf("descriptors: %v", err)
	}
	rules, err := c.Resource(ctx, "em/rules")
	if err != nil {
		b.Fatalf("em/rules: %v (is a pipeline with an ExactMatch module named em applied?)", err)
	}
	keyMD, err := liveMessage(files, "bess.pb.ExactMatchRuleKey")
	if err != nil {
		b.Fatalf("ExactMatchRuleKey: %v", err)
	}
	valueMD, err := liveMessage(files, "bess.pb.ExactMatchRuleValue")
	if err != nil {
		b.Fatalf("ExactMatchRuleValue: %v", err)
	}

	s := &liveShape{client: c, rules: rules}
	s.keys = make([]proto.Message, liveAddrs)
	s.values = make([]proto.Message, liveAddrs+1)
	for i := range s.keys {
		s.keys[i] = liveKey(b, keyMD, i)
	}
	for i := range s.values {
		s.values[i] = liveValue(b, valueMD, 1+i%liveGates)
	}
	return s
}

// liveMessage is one message type out of the daemon's schema: a descriptor name
// resolves to a descriptor, and the benchmark needs the message one.
func liveMessage(files *protoregistry.Files, name protoreflect.FullName) (protoreflect.MessageDescriptor, error) {
	d, err := files.FindDescriptorByName(name)
	if err != nil {
		return nil, err
	}
	md, ok := d.(protoreflect.MessageDescriptor)
	if !ok {
		return nil, fmt.Errorf("%s is not a message", name)
	}
	return md, nil
}

// liveKey is one rule key: a 4-byte address in this benchmark's own range, so
// it cannot disturb the C++ bench's live set, in the bytes field the
// ExactMatch module matches on.
func liveKey(b *testing.B, md protoreflect.MessageDescriptor, address int) proto.Message {
	b.Helper()
	var raw [4]byte
	binary.BigEndian.PutUint32(raw[:], liveBase+uint32(address))
	m := dynamicpb.NewMessage(md)
	json := `{"fields": [{"value_bin": "` + base64.StdEncoding.EncodeToString(raw[:]) + `"}]}`
	if err := protojson.Unmarshal([]byte(json), m); err != nil {
		b.Fatalf("key %d: %v", address, err)
	}
	return m
}

// liveValue is one rule value: a gate number, so the rules steer somewhere.
func liveValue(b *testing.B, md protoreflect.MessageDescriptor, gate int) proto.Message {
	b.Helper()
	m := dynamicpb.NewMessage(md)
	if err := protojson.Unmarshal([]byte(fmt.Sprintf(`{"gate": %d}`, gate)), m); err != nil {
		b.Fatalf("value %d: %v", gate, err)
	}
	return m
}

// BenchmarkLiveChurn is k upserts and k erases per transaction: 2, 8, 16 and 32
// operations, the shapes the C++ bench sweeps and the size of a session's own
// transaction, on the same daemon and the same live-matching traffic.
func BenchmarkLiveChurn(b *testing.B) {
	for _, k := range []int{1, 4, 8, 16} {
		b.Run(fmt.Sprintf("%dops", 2*k), func(b *testing.B) {
			s := newLiveShape(b)
			ctx := context.Background()
			busyBefore := liveBusy(b, s.client)
			// The erase pool is loaded before timing, as the C++ bench starts
			// with --sessions rules live: a transaction must erase rules that
			// exist and never the ones it inserts.
			preload := s.client.Transaction()
			for i := 0; i < livePreload; i++ {
				if err := preload.Upsert(s.rules, s.keys[i], s.values[i]); err != nil {
					b.Fatal(err)
				}
			}
			if _, err := preload.Commit(ctx); err != nil {
				b.Fatal(err)
			}
			var latency []time.Duration
			b.ReportAllocs()
			b.ResetTimer()
			next, oldest := livePreload, 0
			for b.Loop() {
				tx := s.client.Transaction()
				for i := 0; i < k; i++ {
					if err := tx.Upsert(s.rules, s.keys[(next+i)%liveAddrs], s.values[(next+i)%liveAddrs]); err != nil {
						b.Fatal(err)
					}
				}
				for i := 0; i < k; i++ {
					if err := tx.Erase(s.rules, s.keys[(oldest+i)%liveAddrs]); err != nil {
						b.Fatal(err)
					}
				}
				t0 := time.Now()
				if _, err := tx.Commit(ctx); err != nil {
					b.Fatal(err)
				}
				latency = append(latency, time.Since(t0))
				next += k
				oldest += k
			}
			b.StopTimer()
			reportChurn(b, latency, 2*k, liveBusy(b, s.client)-busyBefore)
		})
	}
}

// reportChurn is the C++ bench's report, plus the operations each transaction
// carried: without that column the percentile cannot be normalised. No
// transactions-per-second metric is reported because b.Elapsed covers the
// setup too, so it would not be the commit loop's rate that the C++ bench
// prints.
func reportChurn(b *testing.B, latency []time.Duration, ops int, busy float64) {
	if len(latency) == 0 {
		return
	}
	sort.Slice(latency, func(i, j int) bool { return latency[i] < latency[j] })
	p50 := latency[len(latency)/2]
	p99 := latency[min(len(latency)-1, len(latency)*99/100)]
	b.ReportMetric(float64(ops), "ops/tx")
	b.ReportMetric(float64(p50.Nanoseconds())/1e3, "p50-us")
	b.ReportMetric(float64(p99.Nanoseconds())/1e3, "p99-us")
	b.ReportMetric(busy, "busy")
}

// liveBusy is the daemon's final busy answers so far, the counter the C++ bench
// reads: answers the daemon retried itself are not counted, so zero here does
// not mean no busy was ever met.
func liveBusy(b *testing.B, c *Client) float64 {
	b.Helper()
	samples, err := c.Metrics(context.Background())
	if err != nil {
		b.Fatalf("metrics: %v", err)
	}
	for _, s := range samples {
		if l := s.GetLabels(); s.GetName() == "bess_transactions_total" &&
			l["outcome"] == "busy" {
			return s.GetValue()
		}
	}
	return 0
}
