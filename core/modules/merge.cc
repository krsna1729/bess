// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "merge.h"

void Merge::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  RunNextModule(ctx, batch);
}

ADD_MODULE(Merge, "merge", "All input gates go out of a single output gate")
