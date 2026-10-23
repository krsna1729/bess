// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "hash_lb.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "arch/crc32c.h"
#include "classifier/runtime_schema.h"
#include "dataplane/member_select.h"
#include "event.h"
#include "metadata.h"
#include "packet_parse.h"
#include "snbuf_layout.h"
#include "utils/ether.h"
#include "utils/format.h"
#include "utils/logging.h"

namespace classifier = bess::classifier;

static inline uint32_t hash_16(uint16_t val, uint32_t init_val) {
  return bess::arch::Crc32c(val, init_val);
}

static inline uint32_t hash_32(uint32_t val, uint32_t init_val) {
  return bess::arch::Crc32c(val, init_val);
}

// Returns a value in [0, range) as a function of an opaque number: the shared
// range mapping (M16, D-066), which computes exactly what the floating-point
// form here did (floor(hash * range / 2^32)), so no flow changes gate.
static inline uint16_t hash_range(uint32_t hashval, uint16_t range) {
  return static_cast<uint16_t>(bess::dataplane::RangeSelect(hashval, range));
}

// A header field at any alignment.
template <typename T>
static inline T Load(const char *p) {
  T v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// The L3 and L4 modes' hash of a frame, over the checked parse ConnTrack,
// Router and Firewall read (D-188): up to two VLAN tags, IPv4 options, IPv6
// extension headers, the lengths checked. False for a frame with nothing to
// hash (not IP, or malformed): the module has no gate for it and drops it, as
// it drops a frame too short for any mode.
//
// The mix is the one these modes always had, over parsed fields: CRC32C of
// the source and destination address words XORed (an IPv6 address folds its
// four words; an IPv4 one is its one word, the parse zeroes the rest), in L4
// mode XORed with both ports as on the wire and the protocol. XOR keeps it
// symmetric -- a flow's two directions share a gate -- and an untagged IPv4
// TCP or UDP packet hashes as it always did. route::FlowHash, the Router's
// group hash, is not symmetric, so it is not used here. A fragment hashes as
// in L3 mode in either mode: a later fragment has no ports, so the first
// fragment's are left out too and one datagram's fragments share a gate. ICMP
// has no ports to add (the parse's two echo-identifier fields cancel).
//
// The common frame (untagged IPv4 without options) is read at its fixed
// offsets after the parse's own checks (CheckFastIpv4), so it hashes and
// drops exactly as through a ParsedFrame without building one.
//
// ParsedFlowHashGeneral is the path for every frame but the common one.
// (Out of line it measured slower on the common frame, M.hashlb +17-20%
// against +6% inline.)
template <bool kL4>
static inline bool ParsedFlowHashGeneral(std::span<const uint8_t> frame, size_t total_len,
                                         uint32_t *hash) {
  using bess::packet::ParseStatus;
  bess::packet::ParsedFrame p;
  const ParseStatus status = bess::packet::ParseFrame(frame, p, total_len);
  if (status != ParseStatus::kOk && status != ParseStatus::kFragment) {
    return false;
  }
  uint32_t words[8];
  std::memcpy(words, p.src.data(), 16);
  std::memcpy(words + 4, p.dst.data(), 16);
  uint32_t v = words[0] ^ words[1] ^ words[2] ^ words[3] ^ words[4] ^ words[5] ^
               words[6] ^ words[7];
  if (kL4 && status == ParseStatus::kOk && !p.first_fragment) {
    v ^= uint32_t{__builtin_bswap16(p.src_port)} ^
         uint32_t{__builtin_bswap16(p.dst_port)} ^ p.protocol;
  }
  *hash = hash_32(v, 0);
  return true;
}

template <bool kL4>
static inline bool ParsedFlowHash(std::span<const uint8_t> frame, size_t total_len,
                                  uint32_t *hash) {
  using bess::packet::ParseStatus;
  const bess::packet::FastIpv4 f = bess::packet::CheckFastIpv4(frame, total_len);
  if (!f.taken) [[unlikely]] {
    return ParsedFlowHashGeneral<kL4>(frame, total_len, hash);
  }
  if (f.status != ParseStatus::kOk && f.status != ParseStatus::kFragment) {
    return false;
  }
  const char *b = reinterpret_cast<const char *>(frame.data());
  uint32_t v = Load<uint32_t>(b + bess::packet::kFastIpv4Src) ^
               Load<uint32_t>(b + bess::packet::kFastIpv4Dst);
  if (kL4 && f.status == ParseStatus::kOk && !f.first_fragment) {
    if (f.protocol == 6 || f.protocol == 17) {
      v ^= uint32_t{Load<uint16_t>(b + bess::packet::kFastIpv4L4)} ^
           uint32_t{Load<uint16_t>(b + bess::packet::kFastIpv4L4 + 2)};
    }
    v ^= f.protocol;
  }
  *hash = hash_32(v, 0);
  return true;
}

bool HashLB::FrameHash(std::span<const uint8_t> frame, size_t total_len, bool l4,
                       uint32_t *hash) {
  return l4 ? ParsedFlowHash<true>(frame, total_len, hash)
            : ParsedFlowHash<false>(frame, total_len, hash);
}

// "settings" (HashLBSettings) holds mode, fields and gates; the class has no
// actions.
const Commands HashLB::cmds = {};

const ResourceTables HashLB::resource_tables = {
    ResourceTable::Singleton<bess::pb::HashLBSettings>("settings")};

HashLB::Config HashLB::Current() const {
  const Config *current = config_.Read();
  return current ? *current : Config{};
}

void HashLB::Install(Config config) {
  auto next = std::make_unique<const Config>(std::move(config));
  if (config_.Read() == nullptr) {
    config_.Initialize(std::move(next));
    return;
  }
  // Workers keep processing: they see the old or the new configuration for a
  // whole batch, and the old one is freed after a grace period.
  config_.Publish(std::move(next));
  init_context().rcu().ReclaimReady();
}

CommandResponse HashLB::ResolveAttr(const std::string &name, size_t size,
                                    int *attr_id, bool allow_new) {
  for (const ResolvedAttr &attr : resolved_attrs_) {
    if (attr.name != name) {
      continue;
    }
    if (attr.size != size) {
      return CommandFailure(EINVAL,
                            "attribute '%s' is %zu bytes, not %zu",
                            name.c_str(), attr.size, size);
    }
    *attr_id = attr.attr_id;
    return CommandSuccess();
  }
  if (!allow_new) {
    return CommandFailure(
        EINVAL,
        "attribute '%s' is not registered: a live update cannot add one "
        "(declare it when the module is created)",
        name.c_str());
  }
  const int id = AddMetadataAttr(name, size,
                                 bess::metadata::Attribute::AccessMode::kRead);
  if (id < 0) {
    return CommandFailure(-id, "attribute '%s' cannot be read", name.c_str());
  }
  resolved_attrs_.push_back(ResolvedAttr{name, size, id});
  *attr_id = id;
  return CommandSuccess();
}

CommandResponse HashLB::ParseFields(
    const google::protobuf::RepeatedPtrField<bess::pb::Field> &fields,
    std::vector<FieldSpec> *specs, bool allow_new_attrs) {
  if (fields.size() > kMaxFields) {
    return CommandFailure(EINVAL, "HashLB can have at most %d fields",
                          kMaxFields);
  }
  specs->clear();
  specs->reserve(static_cast<size_t>(fields.size()));
  for (int i = 0; i < fields.size(); i++) {
    const bess::pb::Field &f = fields.Get(i);
    const int64_t size = f.num_bytes();
    if (size < 1 || size > kMaxFieldSize) {
      return CommandFailure(EINVAL,
                            "Error adding field %d: 'size' must be in [1,%d]",
                            i, kMaxFieldSize);
    }
    FieldSpec spec;
    spec.size = static_cast<size_t>(size);
    if (f.position_case() == bess::pb::Field::kAttrName) {
      spec.source = classifier::SourceKind::kMetadata;
      CommandResponse err =
          ResolveAttr(f.attr_name(), spec.size, &spec.attr_id, allow_new_attrs);
      if (err.has_error()) {
        return err;
      }
    } else {
      spec.source = classifier::SourceKind::kPacket;
      if (f.offset() > static_cast<uint32_t>(kMaxFieldOffset)) {
        return CommandFailure(EINVAL, "Error adding field %d: invalid 'offset'",
                              i);
      }
      spec.source_offset = f.offset();
    }
    specs->push_back(spec);
  }
  return CommandSuccess();
}

CommandResponse HashLB::BuildExtraction(
    const std::vector<FieldSpec> &specs,
    std::shared_ptr<const FieldExtraction> *extraction) const {
  // Dense key: each field's bytes after the previous ones, in field order.
  // The plan checks each source's extent (BoundsPolicy::kCheck), so a packet
  // too short for a field fails extraction instead of being read past its
  // first segment.
  classifier::RuntimeClassifierSchema schema;
  schema.bounds = classifier::BoundsPolicy::kCheck;
  schema.key_fields.reserve(specs.size());
  size_t key_offset = 0;
  for (size_t i = 0; i < specs.size(); i++) {
    const FieldSpec &spec = specs[i];
    classifier::RuntimeKeyField field;
    field.key_offset = key_offset;
    field.size = spec.size;
    field.source = spec.source;
    if (spec.source == classifier::SourceKind::kPacket) {
      field.source_offset = spec.source_offset;
    } else {
      // Baked in: a metadata offset is assigned by the resume, so the plan is
      // rebuilt then (OnEvent). The check proves the offset is non-negative;
      // mt_offset_t is int8_t, so widen through unsigned char.
      const bess::metadata::mt_offset_t offset =
          attr_offset(static_cast<size_t>(spec.attr_id));
      if (!bess::metadata::IsValidOffset(offset)) {
        return CommandFailure(
            EINVAL, "Error adding field %zu: attribute has no readable offset",
            i);
      }
      field.source_offset = static_cast<size_t>(static_cast<uint8_t>(offset));
    }
    key_offset += spec.size;
    schema.key_fields.push_back(field);
  }
  schema.key_size = key_offset;
  auto plan = classifier::ExtractPlan::Compile(schema);
  if (!plan) {
    return CommandFailure(EINVAL, "Error adding fields: %s",
                          plan.error().message.c_str());
  }
  *extraction =
      std::make_shared<const FieldExtraction>(std::move(*plan), specs.size());
  return CommandSuccess();
}

CommandResponse HashLB::ApplyMode(const bess::pb::HashLBCommandSetModeArg &arg,
                                  Config *config, bool allow_new_attrs) {
  if (arg.fields_size()) {
    std::vector<FieldSpec> specs;
    CommandResponse err = ParseFields(arg.fields(), &specs, allow_new_attrs);
    if (err.has_error()) {
      return err;
    }
    std::shared_ptr<const FieldExtraction> extraction;
    err = BuildExtraction(specs, &extraction);
    if (err.has_error()) {
      return err;
    }
    config->mode = Mode::kOther;
    config->field_specs = std::move(specs);
    config->extraction = std::move(extraction);
  } else if (arg.mode() == "l2") {
    config->mode = Mode::kL2;
  } else if (arg.mode() == "l3") {
    config->mode = Mode::kL3;
  } else if (arg.mode() == "l4") {
    config->mode = Mode::kL4;
  } else {
    return CommandFailure(EINVAL, "available LB modes: l2, l3, l4");
  }
  if (config->mode != Mode::kOther) {
    // The custom tuple is kOther's; the mode replaces it.
    config->field_specs.clear();
    config->extraction.reset();
  }
  config->settings.set_mode(arg.mode());
  *config->settings.mutable_fields() = arg.fields();
  return CommandSuccess();
}

CommandResponse HashLB::ApplyGates(const bess::pb::HashLBCommandSetGatesArg &arg,
                                   Config *config) const {
  // Every packet goes to gates[hash % n]: n == 0 would index an empty list.
  if (arg.gates_size() == 0) {
    return CommandFailure(EINVAL, "HashLB needs at least one ogate");
  }
  if (static_cast<size_t>(arg.gates_size()) > kMaxGates) {
    return CommandFailure(EINVAL, "HashLB can have at most %zu ogates",
                          kMaxGates);
  }
  std::vector<gate_idx_t> gates;
  gates.reserve(static_cast<size_t>(arg.gates_size()));
  for (int i = 0; i < arg.gates_size(); i++) {
    // Check the 64-bit wire value before narrowing: gate_idx_t is 16 bits,
    // so a post-cast check would accept 65536 as gate 0.
    if (!bess::IsValidGateValue(arg.gates(i))) {
      return CommandFailure(EINVAL, "Invalid ogate %llu",
                            static_cast<unsigned long long>(arg.gates(i)));
    }
    gates.push_back(static_cast<gate_idx_t>(arg.gates(i)));
  }
  config->gates = std::move(gates);
  *config->settings.mutable_gates() = arg.gates();
  return CommandSuccess();
}

CommandResponse HashLB::Init(const bess::pb::HashLBArg &arg) {
  Config config;
  bess::pb::HashLBCommandSetGatesArg gates_arg;
  *gates_arg.mutable_gates() = arg.gates();
  CommandResponse ret = ApplyGates(gates_arg, &config);
  if (ret.has_error()) {
    return ret;
  }
  if (arg.mode().size() || arg.fields_size()) {
    bess::pb::HashLBCommandSetModeArg mode_arg;
    mode_arg.set_mode(arg.mode());
    *mode_arg.mutable_fields() = arg.fields();
    ret = ApplyMode(mode_arg, &config, /*allow_new_attrs=*/true);
    if (ret.has_error()) {
      return ret;
    }
  } else {
    config.settings.set_mode("l4");  // kDefaultMode
  }
  Install(std::move(config));

  // A whole new configuration, validated as set_gates then set_mode validate
  // it, published by an RCU swap while workers run: the replaced one is
  // retired against the transaction's grace period.
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [this](const bess::pb::HashLBSettings &v)
          -> std::expected<std::unique_ptr<const Config>, std::string> {
        Config next;
        bess::pb::HashLBCommandSetGatesArg gates_arg;
        *gates_arg.mutable_gates() = v.gates();
        CommandResponse err = ApplyGates(gates_arg, &next);
        if (err.has_error()) {
          return std::unexpected(err.error().errmsg());
        }
        bess::pb::HashLBCommandSetModeArg mode_arg;
        mode_arg.set_mode(v.mode());
        *mode_arg.mutable_fields() = v.fields();
        // A live write does not resume: it cannot register a new attribute
        // (its offset would still be zero), so only Init may do that.
        err = ApplyMode(mode_arg, &next, /*allow_new_attrs=*/false);
        if (err.has_error()) {
          return std::unexpected(err.error().errmsg());
        }
        return std::make_unique<const Config>(std::move(next));
      },
      [this](std::unique_ptr<const Config> &next, bess::dataplane::Retirer &retirer) {
        retirer.Retire(config_.Exchange(std::move(next)));
      },
      bess::dataplane::Resource::Footprint{.retires = 1},
      [this] { return config_.Read()->settings; },
      nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void HashLB::DeInit() {
  settings_.Release();
}

std::string HashLB::GetDesc() const {
  const Config *config = config_.Read();
  return bess::utils::Format(
      "%zu fields",
      config != nullptr && config->extraction != nullptr
          ? config->extraction->num_fields
          : 0);
}

int HashLB::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  const Config *current = config_.Read();
  if (current == nullptr || current->mode != Mode::kOther) {
    return 0;
  }
  // A metadata offset is assigned by the resume, after Init built the first
  // plan against a placeholder; the plan bakes the offset in, so rebuild it.
  Config next = *current;
  std::shared_ptr<const FieldExtraction> extraction;
  CommandResponse err = BuildExtraction(next.field_specs, &extraction);
  if (err.has_error()) {
    // There is no key to hash: fail closed rather than hash a wrong region.
    LOG(ERROR) << "HashLB '" << name() << "': " << err.error().errmsg()
               << "; dropping every packet until the fields are set again";
  }
  next.extraction = std::move(extraction);
  Install(std::move(next));
  return 0;
}

// The custom tuple's hash: CRC32C over the key bytes, zero-padded to whole
// 8-byte words. This is the layout and hash the field table this mode used to
// build produced (fields packed in order, ExactMatchKeyHash over the padded
// words), so no flow changes gate.
static inline uint32_t HashKey(const std::byte *key, size_t key_size) {
  uint32_t hash = 0;
  size_t at = 0;
  for (; at + sizeof(uint64_t) <= key_size; at += sizeof(uint64_t)) {
    hash = bess::arch::Crc32c(
        Load<uint64_t>(reinterpret_cast<const char *>(key) + at), hash);
  }
  if (at < key_size) {
    uint64_t tail = 0;
    std::memcpy(&tail, key + at, key_size - at);
    hash = bess::arch::Crc32c(tail, hash);
  }
  return hash;
}

template <>
inline void HashLB::DoProcessBatch<HashLB::Mode::kOther>(
    Context *ctx, bess::PacketBatch *batch, const Config &config) {
  // Only reached with a plan; ProcessBatch drops everything without one.
  const classifier::ExtractPlan &plan = config.extraction->plan;
  const size_t key_size = plan.key_size();
  CHECK_LE(key_size, kMaxKeyBytes);
  std::array<classifier::SourceView, bess::PacketBatch::kMaxBurst> sources;
  std::array<std::byte, bess::PacketBatch::kMaxBurst * kMaxKeyBytes> keys;
  const size_t cnt = batch->cnt();
  for (size_t i = 0; i < cnt; i++) {
    const bess::PacketRef pkt = batch->packet(i);
    // The first segment only: a field reaching past it (a short packet, or a
    // chained packet's later segment) fails extraction under kCheck rather
    // than being read out of bounds.
    sources[i].packet = classifier::ConstBytes(
        pkt.head_data<const std::byte *>(),
        static_cast<size_t>(pkt.data_len()));
    sources[i].metadata = classifier::ConstBytes(
        pkt.metadata<const std::byte *>(), SNBUF_METADATA);
  }
  // The key is dense (every field's bytes, in order), so a successful
  // extraction writes every byte of the row: no pre-zeroing is needed. A
  // failed extraction writes nothing and the row is not hashed (the packet is
  // dropped).
  const uint64_t extracted = plan.ExecuteBatch(
      std::span<const classifier::SourceView>(sources).first(cnt),
      classifier::MutableBytes(keys.data(), cnt * key_size), key_size);
  const gate_idx_t *gates = config.gates.data();
  const uint16_t num_gates = static_cast<uint16_t>(config.gates.size());
  for (size_t i = 0; i < cnt; i++) {
    const bess::PacketRef pkt = batch->packet(i);
    if (!(extracted & (uint64_t{1} << i))) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    const uint32_t hash = HashKey(keys.data() + i * key_size, key_size);
    EmitPacket(ctx, pkt, gates[hash_range(hash, num_gates)]);
  }
}

template <>
inline void HashLB::DoProcessBatch<HashLB::Mode::kL2>(
    Context *ctx, bess::PacketBatch *batch, const Config &config) {
  const gate_idx_t *gates = config.gates.data();
  const uint16_t num_gates = static_cast<uint16_t>(config.gates.size());
  for (bess::PacketRef snb : batch->packets()) {
    if (unlikely(snb.head_len() <
                 2 * sizeof(bess::utils::Ethernet::Address))) {
      EmitPacket(ctx, snb, DROP_GATE);
      continue;
    }
    uint16_t *parts = snb.head_data<uint16_t *>();
    uint16_t sum = 0;

    for (int j = 0; j < 6; j++) {
      sum ^= parts[j]; /* xor with next two bytes of MAC addresses starting at
                          offset j of Ethernet header */
    }

    uint32_t hash_val = hash_16(sum, 0);

    EmitPacket(ctx, snb, gates[hash_range(hash_val, num_gates)]);
  }
}

template <>
inline void HashLB::DoProcessBatch<HashLB::Mode::kL3>(
    Context *ctx, bess::PacketBatch *batch, const Config &config) {
  const gate_idx_t *gates = config.gates.data();
  const uint16_t num_gates = static_cast<uint16_t>(config.gates.size());
  for (bess::PacketRef snb : batch->packets()) {
    uint32_t hash_val;
    if (unlikely(!ParsedFlowHash</*l4=*/false>(
            std::span(snb.head_data<const uint8_t *>(), snb.head_len()), snb.total_len(),
            &hash_val))) {
      EmitPacket(ctx, snb, DROP_GATE);
      continue;
    }
    EmitPacket(ctx, snb, gates[hash_range(hash_val, num_gates)]);
  }
}

template <>
inline void HashLB::DoProcessBatch<HashLB::Mode::kL4>(
    Context *ctx, bess::PacketBatch *batch, const Config &config) {
  const gate_idx_t *gates = config.gates.data();
  const uint16_t num_gates = static_cast<uint16_t>(config.gates.size());
  for (bess::PacketRef snb : batch->packets()) {
    uint32_t hash_val;
    if (unlikely(!ParsedFlowHash</*l4=*/true>(
            std::span(snb.head_data<const uint8_t *>(), snb.head_len()), snb.total_len(),
            &hash_val))) {
      EmitPacket(ctx, snb, DROP_GATE);
      continue;
    }
    EmitPacket(ctx, snb, gates[hash_range(hash_val, num_gates)]);
  }
}

void HashLB::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // One configuration for the whole batch.
  const Config &config = *config_.Read();
  switch (config.mode) {
    case Mode::kL2:
      DoProcessBatch<Mode::kL2>(ctx, batch, config);
      break;
    case Mode::kL3:
      DoProcessBatch<Mode::kL3>(ctx, batch, config);
      break;
    case Mode::kL4:
      DoProcessBatch<Mode::kL4>(ctx, batch, config);
      break;
    case Mode::kOther:
      if (config.extraction == nullptr) {
        // Fail closed (see OnEvent): no key, so no packet has a gate.
        for (bess::PacketRef pkt : batch->packets()) {
          EmitPacket(ctx, pkt, DROP_GATE);
        }
      } else {
        DoProcessBatch<Mode::kOther>(ctx, batch, config);
      }
      break;
    default:
      DCHECK(0);
  }
}

ADD_MODULE(HashLB, "hash_lb",
           "splits packets on a flow basis with L2/L3/L4 header fields")
