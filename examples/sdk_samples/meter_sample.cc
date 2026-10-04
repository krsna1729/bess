// SPDX-License-Identifier: BSD-3-Clause

// Meters (meter/meter_set.h): build a generation of meters, colour a batch,
// reconfigure, and handle a bad profile. Ownership: the control side edits a
// MeterSetBuilder and publishes immutable MeterSet generations; the packet path
// holds a `const MeterSet *` and never sees the builder. A meter state lives as
// long as some generation references it, so a reconfiguration never pulls
// state from under a running worker.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

#include "meter/meter_set.h"

namespace sample {

using bess::meter::MeterColor;
using bess::meter::MeterErrorName;
using bess::meter::MeterId;
using bess::meter::MeterSetBuilder;
using bess::meter::MeterSharing;
using bess::meter::SrTcmSpec;
using bess::meter::TrTcmSpec;

// `now` is a TSC timestamp (rte_rdtsc()); the EAL must be up, as in bessd.
bool MeterSample(uint64_t now) {
  MeterSetBuilder builder(/*capacity=*/16);
  const TrTcmSpec gold{.committed_rate = 1'000'000, .committed_burst = 10'000,
                       .peak_rate = 2'000'000, .peak_burst = 20'000};
  // A per-session meter runs on one worker: exclusive, no atomics.
  if (auto added = builder.Add(MeterId{1}, gold, MeterSharing::kWorkerExclusive); !added) {
    std::fprintf(stderr, "add: %s\n", MeterErrorName(added.error()));
    return false;
  }
  // A malformed profile is refused with a reason; nothing changes.
  const SrTcmSpec broken{.committed_rate = 0, .committed_burst = 1, .excess_burst = 1};
  if (auto bad = builder.Add(MeterId{2}, broken, MeterSharing::kShared); bad) {
    return false;
  }
  std::unique_ptr<const bess::meter::MeterSet> generation = builder.Build();

  // Typed hot path: one batch of ids and sizes, colours out.
  const std::array<MeterId, 2> ids = {MeterId{1}, MeterId{9}};
  const std::array<uint32_t, 2> bytes = {1500, 1500};
  std::array<MeterColor, 2> colors{};
  const uint64_t resolved = generation->CheckBatch(ids, bytes, colors, now);
  // Bit 0: meter 1 coloured the packet; bit 1 unset: there is no meter 9.
  if (resolved != 0b01) {
    return false;
  }
  // An update is a new generation; the old one stays valid until retired.
  if (!builder.Reconfigure(MeterId{1}, TrTcmSpec{gold.committed_rate * 2, gold.committed_burst,
                                                gold.peak_rate * 2, gold.peak_burst})) {
    return false;
  }
  std::unique_ptr<const bess::meter::MeterSet> next = builder.Build();
  return next->Lookup(MeterId{1}) != nullptr && builder.Erase(MeterId{1}).has_value();
}

}  // namespace sample
