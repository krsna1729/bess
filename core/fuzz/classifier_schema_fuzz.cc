// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes the runtime classifier schema: RuntimeClassifierSchema::Validate(),
// ExtractPlan::Compile/Execute/ExecuteBatch and ResultPlan::Compile/Apply/
// ApplyBatch (core/classifier/{runtime_schema,extract_plan,result_plan}.h).
//
// Input (FuzzInput, all little-endian; reads past the end are zero):
//   u8 key_size (% 72), u8 value_size (% 72), u8 flags (bit0: kCheck, else
//   kAssumeAvailable),
//   u8 key field count (% 9), per field:
//     u8 ctl  bits0-1 source (raw: 2,3 are invalid kinds), bits2-3 mask mode
//             (1: mask of `size` bytes, 2: mask of the wrong size), bit4/5/6:
//             source_offset / key_offset / size near SIZE_MAX
//     u8 source_offset, u8 key_offset, u8 size (% 40), mask bytes (mode 1/2)
//   u8 result field count (% 6), per field:
//     u8 ctl  bit0/1/2: value_offset / destination_offset / size near SIZE_MAX
//     u8 value_offset, u8 destination_offset, u8 size (% 40)
//   then a 64-byte control block (zero-padded when the input is short):
//     u8 packet length, u8 metadata length, u8 key buffer slack (% 5; the key
//     span is key_size - 1 .. key_size + 3), u8 batch size (1 + % 8),
//     u8 key stride slack (% 8), per batch item: u8 packet trim, u8 metadata
//     trim (trim = byte - 191 when byte >= 192, else 0; kCheck only),
//     u8 value length slack (% 5, as for the key), u8 metadata buffer control
//     (bit7: just past the furthest destination + low 2 bits, else the
//     byte is the length), u8 result batch size (1 + % 4), per result item:
//     u8 value trim, u8 metadata trim,
//   rest: content bytes cycled into packets, metadata and values.
//
// Oracle:
//   - Validate() accepts exactly what an independent reference accepts
//     (occupancy/pairwise overlap checks with overflow-safe arithmetic), and
//     both Compile()s succeed iff Validate() does, preserving key_size,
//     value_size and bounds; fully_covers_key() == every key byte covered.
//   - Execute == a naive per-field copy+mask into the key, nothing written
//     past key_size or into gaps (pattern-filled buffers), nothing written
//     on failure; kCheck rejects exactly when some field leaves its source
//     span or the key span is short. kAssumeAvailable is only exercised with
//     sources that cover every field (its contract).
//   - ExecuteBatch: same per row; the returned mask has bit i iff row i
//     succeeded; stride slack, gaps, failed rows and canaries around the
//     output are untouched; each row equals per-item Execute.
//   - Apply / ApplyBatch == naive per-field copy; on failure no byte outside
//     the configured destinations changes (each destination byte is old or
//     new); ApplyBatch succeeds iff every per-item Apply does.
//   Every source/value/key/metadata span is its own exact-size heap vector,
//   so ASan flags any access outside it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "classifier/extract_plan.h"
#include "classifier/result_plan.h"
#include "classifier/runtime_schema.h"
#include "fuzz/fuzz_support.h"

namespace {

using bess::classifier::BoundsPolicy;
using bess::classifier::ConstBytes;
using bess::classifier::ExtractPlan;
using bess::classifier::MutableBytes;
using bess::classifier::ResultPlan;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeKeyField;
using bess::classifier::RuntimeResultField;
using bess::classifier::SourceKind;
using bess::classifier::SourceView;
using bess::fuzz::FuzzInput;

using Bytes = std::vector<std::byte>;

constexpr size_t kMax = std::numeric_limits<size_t>::max();
// Sources/metadata larger than this are not materialized (kAssumeAvailable
// runs are skipped instead).
constexpr size_t kMaterializeLimit = 4096;
constexpr size_t kCanary = 16;

size_t Huge(uint8_t low) { return kMax - low; }

// Overflow-safe "[offset, offset + size) lies within [0, limit)".
bool Within(size_t offset, size_t size, size_t limit) {
  return offset <= limit && size <= limit - offset;
}

// End of [offset, offset + size), or nullopt when it does not fit in size_t.
std::optional<size_t> End(size_t offset, size_t size) {
  size_t end;
  if (__builtin_add_overflow(offset, size, &end)) {
    return std::nullopt;
  }
  return end;
}

std::byte Pattern(size_t i, uint8_t salt) {
  return static_cast<std::byte>((i * 131 + salt * 29 + 7) & 0xff);
}

void Fill(Bytes &bytes, uint8_t salt) {
  for (size_t i = 0; i < bytes.size(); i++) {
    bytes[i] = Pattern(i, salt);
  }
}

// Content source: cycles through the rest of the input, varied per use.
class Content {
 public:
  explicit Content(std::span<const uint8_t> bytes) : bytes_(bytes) {}
  Bytes Make(size_t length, uint8_t salt) const {
    Bytes out(length);
    for (size_t i = 0; i < length; i++) {
      const uint8_t base = bytes_.empty()
                               ? static_cast<uint8_t>(i * 7 + 1)
                               : bytes_[(i + salt) % bytes_.size()];
      out[i] = static_cast<std::byte>(base ^ (salt * 0x3b));
    }
    return out;
  }

 private:
  std::span<const uint8_t> bytes_;
};

// ---- Reference model -------------------------------------------------------

bool RefValidate(const RuntimeClassifierSchema &s) {
  if (s.key_size == 0) {
    return false;
  }
  std::vector<bool> used(s.key_size, false);
  for (const RuntimeKeyField &f : s.key_fields) {
    if (f.size == 0) {
      return false;
    }
    if (f.source != SourceKind::kPacket && f.source != SourceKind::kMetadata) {
      return false;
    }
    if (!Within(f.key_offset, f.size, s.key_size)) {
      return false;
    }
    if (!f.normalization.mask.empty() && f.normalization.mask.size() != f.size) {
      return false;
    }
  }
  // Key overlap via an occupancy map (key_size is small here).
  for (const RuntimeKeyField &f : s.key_fields) {
    for (size_t k = 0; k < f.size; k++) {
      if (used[f.key_offset + k]) {
        return false;
      }
      used[f.key_offset + k] = true;
    }
  }
  for (const RuntimeResultField &f : s.result_fields) {
    if (f.size == 0 || !Within(f.value_offset, f.size, s.value_size) ||
        !End(f.destination_offset, f.size)) {
      return false;
    }
  }
  // Destination overlap: two half-open ranges intersect iff the later start
  // precedes the earlier end.
  for (size_t i = 0; i < s.result_fields.size(); i++) {
    for (size_t j = 0; j < i; j++) {
      const RuntimeResultField &a = s.result_fields[i];
      const RuntimeResultField &b = s.result_fields[j];
      const size_t start = std::max(a.destination_offset, b.destination_offset);
      const size_t end = std::min(*End(a.destination_offset, a.size),
                                  *End(b.destination_offset, b.size));
      if (start < end) {
        return false;
      }
    }
  }
  return true;
}

bool RefFullyCovers(const RuntimeClassifierSchema &s) {
  size_t covered = 0;
  for (const RuntimeKeyField &f : s.key_fields) {
    covered += f.size;  // Valid schema: disjoint and within key_size.
  }
  return covered == s.key_size;
}

const Bytes &SourceOf(const RuntimeKeyField &f, const Bytes &packet,
                      const Bytes &metadata) {
  return f.source == SourceKind::kPacket ? packet : metadata;
}

// Bytes each source must hold to cover every field, nullopt if unbounded.
std::optional<size_t> RequiredBytes(const RuntimeClassifierSchema &s,
                                    SourceKind kind) {
  size_t need = 0;
  for (const RuntimeKeyField &f : s.key_fields) {
    if (f.source != kind) {
      continue;
    }
    const auto end = End(f.source_offset, f.size);
    if (!end) {
      return std::nullopt;
    }
    need = std::max(need, *end);
  }
  return need;
}

// Naive extraction into `key` (a key_size-or-larger row). Returns false and
// writes nothing when the plan must reject.
bool RefExtract(const RuntimeClassifierSchema &s, const Bytes &packet,
                const Bytes &metadata, std::span<std::byte> key) {
  if (key.size() < s.key_size) {
    return false;
  }
  for (const RuntimeKeyField &f : s.key_fields) {
    const Bytes &src = SourceOf(f, packet, metadata);
    if (!Within(f.source_offset, f.size, src.size())) {
      // Under kAssumeAvailable the harness never gets here (see caller).
      FUZZ_CHECK(s.bounds == BoundsPolicy::kCheck);
      return false;
    }
  }
  for (const RuntimeKeyField &f : s.key_fields) {
    const Bytes &src = SourceOf(f, packet, metadata);
    for (size_t k = 0; k < f.size; k++) {
      std::byte b = src[f.source_offset + k];
      if (!f.normalization.mask.empty()) {
        b &= f.normalization.mask[k];
      }
      key[f.key_offset + k] = b;
    }
  }
  return true;
}

bool RefApplyFits(const RuntimeClassifierSchema &s, size_t value_length,
                  size_t metadata_length) {
  if (value_length < s.value_size) {
    return false;
  }
  for (const RuntimeResultField &f : s.result_fields) {
    if (!Within(f.destination_offset, f.size, metadata_length)) {
      return false;
    }
  }
  return true;
}

void RefApply(const RuntimeClassifierSchema &s, const Bytes &value,
              Bytes &metadata) {
  for (const RuntimeResultField &f : s.result_fields) {
    for (size_t k = 0; k < f.size; k++) {
      metadata[f.destination_offset + k] = value[f.value_offset + k];
    }
  }
}

// After a failed Apply: bytes outside every destination are untouched and
// each destination byte holds its old or its new value.
void CheckFailedApply(const RuntimeClassifierSchema &s, const Bytes &value,
                      const Bytes &before, const Bytes &after) {
  std::vector<int> source(after.size(), -1);  // value index or -1
  for (const RuntimeResultField &f : s.result_fields) {
    for (size_t k = 0; k < f.size; k++) {
      const size_t at = f.destination_offset + k;  // may be past the end
      if (at < after.size() && f.value_offset + k < value.size()) {
        source[at] = static_cast<int>(f.value_offset + k);
      }
    }
  }
  for (size_t i = 0; i < after.size(); i++) {
    if (after[i] == before[i]) {
      continue;
    }
    FUZZ_CHECK(source[i] >= 0);
    FUZZ_CHECK(after[i] == value[static_cast<size_t>(source[i])]);
  }
}

// ---- Decoding --------------------------------------------------------------

RuntimeClassifierSchema DecodeSchema(FuzzInput &in) {
  RuntimeClassifierSchema s;
  s.key_size = in.U8() % 72;
  s.value_size = in.U8() % 72;
  s.bounds = (in.U8() & 1) != 0 ? BoundsPolicy::kCheck
                                : BoundsPolicy::kAssumeAvailable;
  const size_t key_fields = in.U8() % 9;
  for (size_t i = 0; i < key_fields; i++) {
    const uint8_t ctl = in.U8();
    RuntimeKeyField f;
    f.source = static_cast<SourceKind>(ctl & 3);
    const uint8_t source_offset = in.U8();
    const uint8_t key_offset = in.U8();
    const uint8_t size = in.U8();
    f.source_offset = (ctl & 0x10) != 0 ? Huge(source_offset) : source_offset;
    f.key_offset = (ctl & 0x20) != 0 ? Huge(key_offset) : key_offset;
    f.size = (ctl & 0x40) != 0 ? Huge(size) : size % 40;
    const unsigned mask_mode = (ctl >> 2) & 3;
    if ((mask_mode == 1 || mask_mode == 2) && f.size < 64) {
      const size_t mask_size = mask_mode == 1 ? f.size : f.size + 1;
      for (size_t k = 0; k < mask_size; k++) {
        f.normalization.mask.push_back(static_cast<std::byte>(in.U8()));
      }
    }
    s.key_fields.push_back(std::move(f));
  }
  const size_t result_fields = in.U8() % 6;
  for (size_t i = 0; i < result_fields; i++) {
    const uint8_t ctl = in.U8();
    const uint8_t value_offset = in.U8();
    const uint8_t destination_offset = in.U8();
    const uint8_t size = in.U8();
    RuntimeResultField f;
    f.value_offset = (ctl & 1) != 0 ? Huge(value_offset) : value_offset;
    f.destination_offset =
        (ctl & 2) != 0 ? Huge(destination_offset) : destination_offset;
    f.size = (ctl & 4) != 0 ? Huge(size) : size % 40;
    s.result_fields.push_back(f);
  }
  return s;
}

size_t Trim(size_t length, uint8_t trim) {
  const size_t cut = trim >= 192 ? trim - 191 : 0;
  return length > cut ? length - cut : 0;
}

// ---- Extraction ------------------------------------------------------------

void FuzzExtract(const RuntimeClassifierSchema &s, const ExtractPlan &plan,
                 FuzzInput &in, const Content &content) {
  const bool check = s.bounds == BoundsPolicy::kCheck;
  const auto need_packet = RequiredBytes(s, SourceKind::kPacket);
  const auto need_metadata = RequiredBytes(s, SourceKind::kMetadata);
  const bool materializable = need_packet && need_metadata &&
                              *need_packet <= kMaterializeLimit &&
                              *need_metadata <= kMaterializeLimit;

  size_t packet_length = in.U8();
  size_t metadata_length = in.U8();
  const size_t key_length = s.key_size + in.U8() % 5;  // key_size - 1 .. + 3
  const size_t batch = 1 + in.U8() % 8;
  const size_t stride = s.key_size + in.U8() % 8;

  if (!check) {
    // kAssumeAvailable: the caller guarantees every source range.
    if (!materializable) {
      return;
    }
    packet_length = std::max(packet_length, *need_packet);
    metadata_length = std::max(metadata_length, *need_metadata);
  }

  // Single Execute.
  {
    const Bytes packet = content.Make(packet_length, 1);
    const Bytes metadata = content.Make(metadata_length, 2);
    Bytes key(key_length == 0 ? 0 : key_length - 1);
    Fill(key, 3);
    Bytes expected = key;
    const bool want = RefExtract(s, packet, metadata, expected);
    const bool got =
        plan.Execute(SourceView{ConstBytes(packet), ConstBytes(metadata)},
                     MutableBytes(key));
    FUZZ_CHECK(got == want);
    FUZZ_CHECK(key == expected);
  }

  // ExecuteBatch versus per-item reference and per-item Execute.
  std::vector<Bytes> packets;
  std::vector<Bytes> metadatas;
  packets.reserve(batch);
  metadatas.reserve(batch);
  std::vector<SourceView> views;
  for (size_t i = 0; i < batch; i++) {
    const uint8_t trim_packet = in.U8();
    const uint8_t trim_metadata = in.U8();
    const size_t p = check ? Trim(packet_length, trim_packet) : packet_length;
    const size_t m =
        check ? Trim(metadata_length, trim_metadata) : metadata_length;
    packets.push_back(content.Make(p, static_cast<uint8_t>(10 + i)));
    metadatas.push_back(content.Make(m, static_cast<uint8_t>(40 + i)));
  }
  for (size_t i = 0; i < batch; i++) {
    views.push_back({ConstBytes(packets[i]), ConstBytes(metadatas[i])});
  }

  Bytes output(kCanary + batch * stride + kCanary);
  Fill(output, 5);
  Bytes expected = output;
  uint64_t want_mask = 0;
  for (size_t i = 0; i < batch; i++) {
    std::span<std::byte> row =
        std::span<std::byte>(expected).subspan(kCanary + i * stride, stride);
    if (RefExtract(s, packets[i], metadatas[i], row)) {
      want_mask |= uint64_t{1} << i;
    }
  }
  const uint64_t got_mask = plan.ExecuteBatch(
      views, MutableBytes(output).subspan(kCanary, batch * stride), stride);
  FUZZ_CHECK(got_mask == want_mask);
  FUZZ_CHECK(output == expected);

  for (size_t i = 0; i < batch; i++) {
    Bytes key(s.key_size);
    for (size_t k = 0; k < key.size(); k++) {
      key[k] = Pattern(kCanary + i * stride + k, 5);
    }
    const bool ok = plan.Execute(views[i], MutableBytes(key));
    FUZZ_CHECK(ok == (((got_mask >> i) & 1) != 0));
    FUZZ_CHECK(std::equal(key.begin(), key.end(),
                          output.begin() + kCanary + i * stride));
  }
}

// ---- Result placement ------------------------------------------------------

void FuzzResult(const RuntimeClassifierSchema &s, const ResultPlan &plan,
                FuzzInput &in, const Content &content) {
  const size_t value_length = s.value_size + in.U8() % 5;  // -1 .. +3, below
  const uint8_t metadata_ctl = in.U8();
  size_t metadata_length = metadata_ctl;
  if ((metadata_ctl & 0x80) != 0) {
    size_t furthest = 0;
    for (const RuntimeResultField &f : s.result_fields) {
      const auto end = End(f.destination_offset, f.size);
      furthest = std::max(furthest, end.value_or(kMax));
    }
    if (furthest > kMaterializeLimit) {
      furthest = kMaterializeLimit;
    }
    metadata_length = furthest + (metadata_ctl & 3);
  }
  const size_t base_value = value_length == 0 ? 0 : value_length - 1;

  // Single Apply.
  {
    const Bytes value = content.Make(base_value, 60);
    Bytes metadata(metadata_length);
    Fill(metadata, 61);
    const Bytes before = metadata;
    const bool want = RefApplyFits(s, value.size(), metadata.size());
    const bool got = plan.Apply(ConstBytes(value), MutableBytes(metadata));
    FUZZ_CHECK(got == want);
    if (want) {
      Bytes expected = before;
      RefApply(s, value, expected);
      FUZZ_CHECK(metadata == expected);
    } else {
      CheckFailedApply(s, value, before, metadata);
    }
  }

  // ApplyBatch versus per-item Apply.
  const size_t batch = 1 + in.U8() % 4;
  std::vector<Bytes> values;
  std::vector<Bytes> batch_metadata;
  std::vector<Bytes> befores;
  bool want_all = true;
  for (size_t i = 0; i < batch; i++) {
    const uint8_t trim_value = in.U8();
    const uint8_t trim_metadata = in.U8();
    values.push_back(content.Make(Trim(base_value, trim_value),
                                  static_cast<uint8_t>(70 + i)));
    Bytes metadata(Trim(metadata_length, trim_metadata));
    Fill(metadata, static_cast<uint8_t>(80 + i));
    befores.push_back(metadata);
    batch_metadata.push_back(std::move(metadata));

    Bytes single = befores.back();
    const bool ok = plan.Apply(ConstBytes(values[i]), MutableBytes(single));
    FUZZ_CHECK(ok == RefApplyFits(s, values[i].size(), single.size()));
    want_all = want_all && ok;
  }
  std::vector<ConstBytes> value_spans(values.begin(), values.end());
  std::vector<MutableBytes> metadata_spans;
  for (Bytes &m : batch_metadata) {
    metadata_spans.emplace_back(m);
  }
  const bool got_all = plan.ApplyBatch(value_spans, metadata_spans);
  FUZZ_CHECK(got_all == want_all);
  for (size_t i = 0; i < batch; i++) {
    if (got_all) {
      Bytes expected = befores[i];
      RefApply(s, values[i], expected);
      FUZZ_CHECK(batch_metadata[i] == expected);
    } else {
      CheckFailedApply(s, values[i], befores[i], batch_metadata[i]);
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput in(data, size);
  const RuntimeClassifierSchema schema = DecodeSchema(in);

  // The remaining fixed-size controls come before the content bytes.
  std::array<uint8_t, 64> controls{};
  const auto control_bytes = in.Bytes(controls.size());
  std::copy(control_bytes.begin(), control_bytes.end(), controls.begin());
  const Content content(in.Rest());

  const bool valid = RefValidate(schema);
  FUZZ_CHECK(schema.Validate().has_value() == valid);

  auto extract = ExtractPlan::Compile(schema);
  auto result = ResultPlan::Compile(schema);
  FUZZ_CHECK(extract.has_value() == valid);
  FUZZ_CHECK(result.has_value() == valid);
  if (!valid) {
    return 0;
  }
  FUZZ_CHECK(extract->key_size() == schema.key_size);
  FUZZ_CHECK(extract->bounds() == schema.bounds);
  FUZZ_CHECK(extract->fully_covers_key() == RefFullyCovers(schema));
  FUZZ_CHECK(result->value_size() == schema.value_size);

  FuzzInput controls_in(controls.data(), controls.size());
  FuzzExtract(schema, *extract, controls_in, content);
  FuzzResult(schema, *result, controls_in, content);
  return 0;
}
