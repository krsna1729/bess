// SPDX-License-Identifier: BSD-3-Clause
package bess

import (
	"context"
	"errors"
	"testing"
	"time"

	pb "github.com/krsna1729/bess/sdk/go/controlv2"
)

func TestBusyBackoffRespectsCancellationAndPriorUnknown(t *testing.T) {
	for _, priorUnknown := range []bool{false, true} {
		name := "answered busy"
		if priorUnknown {
			name = "earlier send unanswered"
		}
		t.Run(name, func(t *testing.T) {
			f := &fake{applies: []step{answer(busy, 1)}}
			if priorUnknown {
				f.applies = append([]step{timeout()}, f.applies...)
				f.gets = []step{notKnown(1)}
			}
			h := newHarness(f, 4)
			r, err := h.c.Resource(context.Background(), resource)
			if err != nil {
				t.Fatal(err)
			}
			tx := h.c.Transaction()
			if err := tx.Upsert(r, &pb.GetTransactionRequest{}, &pb.ListTransactionResourcesRequest{}); err != nil {
				t.Fatal(err)
			}
			entered := make(chan struct{})
			h.c.sleep = func(ctx context.Context, _ time.Duration) error {
				close(entered)
				return retryWait(ctx, time.Hour)
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan error, 1)
			go func() { _, err := tx.Commit(ctx); done <- err }()
			select {
			case <-entered:
			case <-time.After(3 * time.Second):
				t.Fatal("did not enter busy backoff")
			}
			cancel()
			select {
			case err := <-done:
				if priorUnknown {
					as[*TransportError](t, err)
				} else if !errors.Is(err, context.Canceled) {
					t.Fatalf("answered busy then cancelled: %v", err)
				}
			case <-time.After(3 * time.Second):
				t.Fatal("cancellation waited for busy backoff")
			}
		})
	}
}
