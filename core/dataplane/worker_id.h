// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_DATAPLANE_WORKER_ID_H_
#define BESS_DATAPLANE_WORKER_ID_H_

#include <cstddef>
#include <cstdint>

#include "dataplane/strong_id.h"

namespace bess {
namespace dataplane {

// A BESS worker's stable logical identity (`Worker::wid()`), as a type (K6,
// the first Phase I step for workers).
//
// It is index-like: zero is the first worker, not an invalid value, and every
// value below kMaxWorkers names a worker slot. It is *not* a CPU id or a DPDK
// lcore id; worker.h explains why those three differ.
struct WorkerIdTag;
using WorkerId = StrongId<WorkerIdTag, uint16_t>;

// Must equal Worker::kMaxWorkers; checked where both are visible
// (stats/current_worker.h) so this header stays free of worker.h.
inline constexpr size_t kMaxWorkers = 64;

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_WORKER_ID_H_
