// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "sink.h"

void Sink::ProcessBatch(Context *, bess::PacketBatch *batch) {
  bess::PacketFreeBatch(batch);
}

ADD_MODULE(Sink, "sink", "discards all packets")
