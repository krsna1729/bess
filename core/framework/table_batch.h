// SPDX-License-Identifier: BSD-3-Clause

// The packet-path half of a module's classifier table (D-189): gather a
// batch's keys with a CompiledKey's ExtractPlan, and place the records its
// matches name with a compiled ResultPlan. Inline, called once per batch
// with the caller's stack scratch: no allocation, no indirect call. The
// control-path half is framework/table_fields.h.

#ifndef BESS_FRAMEWORK_TABLE_BATCH_H_
#define BESS_FRAMEWORK_TABLE_BATCH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "classifier/extract_plan.h"
#include "classifier/record_arena.h"
#include "classifier/result_plan.h"
#include "classifier/table_schema.h"
#include "gate.h"
#include "packet.h"
#include "packet_reshape.h"
#include "pktbatch.h"
#include "snbuf_layout.h"

namespace bess::framework {

// Gathers one key row of key.key_size bytes per packet of `batch` into
// `keys` (room for batch.cnt() rows) and returns the mask of rows extracted.
// A packet is its first segment only, so a field reaching past data_len (or
// into a later segment) fails its row rather than over-reading. A CompiledKey
// is dense (CompileKey refuses a layout with gaps), so an extracted row is
// written whole with nothing zeroed first; failed rows are zeroed, so a table
// that looks every row up sees well-defined keys. Discard failed rows'
// results through the mask.
[[gnu::always_inline]] inline uint64_t ExtractKeys(
    const classifier::CompiledKey &key, const PacketBatch &batch,
    std::byte *keys) noexcept {
  const auto cnt = static_cast<size_t>(batch.cnt());
  const size_t key_size = key.key_size;
  const classifier::MutableBytes rows(keys, cnt * key_size);
  std::array<classifier::SourceView, PacketBatch::kMaxBurst> sources;
  for (size_t i = 0; i < cnt; i++) {
    const PacketRef pkt = batch.packet(i);
    sources[i].packet = classifier::ConstBytes(pkt.head_data<const std::byte *>(),
                                               static_cast<size_t>(pkt.data_len()));
    sources[i].metadata =
        classifier::ConstBytes(pkt.metadata<const std::byte *>(), SNBUF_METADATA);
  }
  const uint64_t ok = key.extract.ExecuteBatch(
      std::span<const classifier::SourceView>(sources).first(cnt), rows, key_size);
  const uint64_t all_valid = (uint64_t{1} << cnt) - 1;
  if ((ok & all_valid) != all_valid) {
    for (size_t i = 0; i < cnt; i++) {
      if ((ok & (uint64_t{1} << i)) == 0) {
        std::memset(keys + (i * key_size), 0, key_size);
      }
    }
  }
  return ok;
}

// Writes record `record_ids[i]` of `records` into packet i of `batch` with
// `plan` (0: nothing to place). A packet the plan writes into is made
// writable first (its handle may be replaced in `batch`); one that cannot be,
// or is shorter than the plan needs, is not written and gets gates[i] =
// DROP_GATE. False when the plan refused the batch, which the checks above
// make unreachable for a valid plan: the caller fails closed rather than emit
// packets whose fields were only partly written.
[[gnu::always_inline]] inline bool PlaceResults(
    const classifier::ResultPlan &plan, const classifier::RecordArena &records,
    PacketBatch *batch, const uint32_t *record_ids, gate_idx_t *gates) noexcept {
  const int cnt = batch->cnt();
  const size_t packet_bytes = plan.required_packet_bytes();
  std::array<classifier::ConstBytes, PacketBatch::kMaxBurst> values;
  std::array<classifier::DestinationView, PacketBatch::kMaxBurst> destinations;
  size_t rows = 0;
  for (int i = 0; i < cnt; i++) {
    if (record_ids[i] == 0) {
      continue;
    }
    PacketRef pkt = batch->packet(i);
    classifier::MutableBytes packet_view;
    if (packet_bytes != 0) {
      PacketHandle handle = pkt.handle();
      if (!packet::EnsureWritable(handle)) {
        gates[i] = DROP_GATE;
        continue;
      }
      batch->handles()[i] = handle;
      pkt = PacketRef(handle);
      if (pkt.data_len() < packet_bytes) {
        gates[i] = DROP_GATE;
        continue;
      }
      packet_view = classifier::MutableBytes(pkt.head_data<std::byte *>(),
                                             static_cast<size_t>(pkt.data_len()));
    }
    values[rows] = records.Lookup(record_ids[i]);
    destinations[rows] = classifier::DestinationView{
        .packet = packet_view,
        .metadata = classifier::MutableBytes(
            reinterpret_cast<std::byte *>(pkt.metadata()), SNBUF_METADATA)};
    rows++;
  }
  return rows == 0 ||
         plan.ApplyBatch(std::span<const classifier::ConstBytes>(values.data(), rows),
                         std::span<const classifier::DestinationView>(
                             destinations.data(), rows));
}

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_TABLE_BATCH_H_
