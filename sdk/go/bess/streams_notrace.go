// SPDX-License-Identifier: BSD-3-Clause
//go:build !transactiontrace

package bess

import "context"

func traceTransaction(context.Context, string, string) {}

func traceFrame(context.Context, string, uint64, []*queuedTransaction) {}
