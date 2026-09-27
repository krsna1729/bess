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

// Transactions per second through the G1.2b engine (Decision D-021), in
// process, for a session-shaped change set: two meters, two actions naming
// them, two exact rules naming the actions -- created in one transaction and
// removed in another. MODERNIZATION.md section 14.5's target is >= 100K
// sessions/s (establish + release); this measures it without RPC.

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "classifier/exact_rule_resource.h"
#include "control/runtime_state.h"
#include "dataplane/slot_resource.h"
#include "dataplane/strong_id.h"
#include "dataplane/transaction_engine.h"

namespace {

using bess::classifier::ConcurrentExactTable;
using bess::classifier::ExactRuleResource;
using namespace bess::dataplane;

struct MeterTag;
struct ActionTag;
using MeterId = StrongId<MeterTag, uint32_t>;
using ActionId = StrongId<ActionTag, uint32_t>;
struct Meter {
  uint64_t cir, cbs;
};
struct Action {
  uint16_t gate;
  MeterId meter;
};

void BM_SessionEstablishRelease(benchmark::State &state) {
  const uint32_t sessions = static_cast<uint32_t>(state.range(0));
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
  auto table = *ConcurrentExactTable::Create(
      8, ConcurrentExactTable::CapacityFor(sessions * 2 + 64), domain);
  SlotTable<MeterId, Meter> meters(sessions * 2 + 2);
  SlotTable<ActionId, Action> actions(sessions * 2 + 2);
  SlotResource<MeterId, Meter> meters_res("meters", 0, meters);
  SlotResource<ActionId, Action> actions_res(
      "actions", 1, actions, [](const Action &a) {
        return std::vector<Reference>{{"meters", EncodeKey(a.meter)}};
      });
  ExactRuleResource rules_res("rules", 2, *table, [](uint64_t v) {
    return std::vector<Reference>{
        {"actions", EncodeKey(ActionId(static_cast<uint32_t>(v)))}};
  });
  TransactionEngine engine(domain);
  engine.Register(&meters_res);
  engine.Register(&actions_res);
  engine.Register(&rules_res);

  // Sessions cycle through the id space; ids freed by a release are reused
  // only after their grace period (no worker is online here, so at once).
  uint32_t s = 0;
  std::vector<Op> establish(6), release(6);
  for (auto _ : state) {
    const uint32_t id = 1 + 2 * (s % sessions);
    const uint64_t key = s % sessions;
    establish[0] = Op::Upsert("meters", EncodeKey(MeterId(id)),
                              std::any(Meter{1000000, 65536}));
    establish[1] = Op::Upsert("meters", EncodeKey(MeterId(id + 1)),
                              std::any(Meter{2000000, 65536}));
    establish[2] = Op::Upsert("actions", EncodeKey(ActionId(id)),
                              std::any(Action{1, MeterId(id)}));
    establish[3] = Op::Upsert("actions", EncodeKey(ActionId(id + 1)),
                              std::any(Action{2, MeterId(id + 1)}));
    establish[4] = Op::Upsert("rules", EncodeKey(key * 2),
                              std::any(uint64_t{id}));
    establish[5] = Op::Upsert("rules", EncodeKey(key * 2 + 1),
                              std::any(uint64_t{id + 1}));
    if (engine.Apply(establish).outcome !=
        TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("establish rejected");
      break;
    }
    release[0] = Op::Erase("rules", EncodeKey(key * 2));
    release[1] = Op::Erase("rules", EncodeKey(key * 2 + 1));
    release[2] = Op::Erase("actions", EncodeKey(ActionId(id)));
    release[3] = Op::Erase("actions", EncodeKey(ActionId(id + 1)));
    release[4] = Op::Erase("meters", EncodeKey(MeterId(id)));
    release[5] = Op::Erase("meters", EncodeKey(MeterId(id + 1)));
    if (engine.Apply(release).outcome != TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("release rejected");
      break;
    }
    s++;
  }
  state.SetItemsProcessed(state.iterations());  // sessions (establish+release)
  domain.Drain();
}
BENCHMARK(BM_SessionEstablishRelease)->Arg(1024)->Arg(65536);

}  // namespace
