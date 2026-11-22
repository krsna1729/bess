// SPDX-License-Identifier: BSD-3-Clause
//go:build transactiontrace

package bess

import (
	"context"
	"runtime/trace"
)

func traceTransaction(ctx context.Context, event, id string) {
	trace.Log(ctx, event, id)
}

func traceFrame(ctx context.Context, event string, attempt uint64, batch []*queuedTransaction) {
	if !trace.IsEnabled() {
		return
	}
	for slot, q := range batch {
		trace.Logf(ctx, event, "%s %d %d %s", batch[0].id, attempt, slot, q.id)
	}
}
