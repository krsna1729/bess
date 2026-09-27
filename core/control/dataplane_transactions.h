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

#ifndef BESS_CONTROL_DATAPLANE_TRANSACTIONS_H_
#define BESS_CONTROL_DATAPLANE_TRANSACTIONS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "control/control_error.h"
#include "dataplane/transaction_engine.h"
#include "pb/control_v2.pb.h"

namespace bess::control {

// The dataplane transaction RPC's server side (G1.2c, Decision D-025), apart
// from gRPC: typed operations decoded through each resource's codec, applied
// through the TransactionEngine, and outcomes recorded by request_id.
//
//   - Idempotency: an outcome is recorded with a digest of the request's
//     contents (everything but request_id). The same id and contents replay
//     the record; the same id with other contents is refused (kConflict).
//     BUSY and CONFLICT outcomes attempted nothing and are not recorded:
//     the client retries under the same id.
//   - Records live in a bounded window, oldest out first (kMaxRecords).
//   - The daemon epoch is drawn at construction: a record of another epoch
//     cannot exist, so a client that sees the epoch change knows an unknown
//     id means "unknown", not "never ran".
//
// Not thread-safe: callers hold the control-plane lock, as for module
// commands, which write the same tables.
class DataplaneTransactions {
 public:
  static constexpr size_t kMaxRecords = 4096;

  explicit DataplaneTransactions(dataplane::TransactionEngine &engine);
  DataplaneTransactions(dataplane::TransactionEngine &engine, uint64_t epoch)
      : engine_(engine), epoch_(epoch) {}

  ControlResult<pb::v2::ApplyTransactionResponse> Apply(
      const pb::v2::ApplyTransactionRequest &request);
  pb::v2::GetTransactionResponse Get(const std::string &request_id) const;
  pb::v2::ListTransactionResourcesResponse List() const;

  uint64_t epoch() const { return epoch_; }
  size_t records() const { return records_.size(); }

 private:
  struct Recorded {
    uint64_t digest;
    pb::v2::TransactionRecord record;
  };

  void Record(const std::string &request_id, uint64_t digest,
              const pb::v2::TransactionRecord &record);

  dataplane::TransactionEngine &engine_;
  const uint64_t epoch_;
  std::unordered_map<std::string, Recorded> records_;
  std::deque<std::string> order_;  // oldest first
};

}  // namespace bess::control

#endif  // BESS_CONTROL_DATAPLANE_TRANSACTIONS_H_
