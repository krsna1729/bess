// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "hash_lb.h"

#include <cstring>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "arch/crc32c.h"
#include "dataplane/member_select.h"
#include "utils/ether.h"
#include "utils/ip.h"

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

// The first segment must hold an Ethernet header and an IPv4 header with a
// sane IHL, plus `after_ip` more bytes (the L4 ports). Returns the IPv4 header
// length in bytes, or 0 when the frame is too short or not IPv4. Hashing a
// frame the module cannot read would read past its head; hash_range needs a
// destination, and a drop is the only answer the module has.
static inline size_t Ipv4HeaderLength(bess::PacketRef pkt, size_t after_ip) {
  const size_t eth_len = sizeof(bess::utils::Ethernet);
  const size_t ip_len = sizeof(bess::utils::Ipv4);
  if (pkt.head_len() < eth_len + ip_len + after_ip) {
    return 0;
  }
  const uint8_t *head = pkt.head_data<const uint8_t *>();
  const unsigned version_ihl = head[eth_len];
  if ((version_ihl >> 4u) != 4u) {
    return 0;
  }
  const size_t ihl = static_cast<size_t>(version_ihl & 0x0fu) * 4;
  if (ihl < ip_len || pkt.head_len() < eth_len + ihl + after_ip) {
    return 0;
  }
  return ihl;
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

CommandResponse HashLB::ApplyMode(const bess::pb::HashLBCommandSetModeArg &arg,
                                  Config *config) const {
  if (arg.fields_size()) {
    auto fields = std::make_shared<ExactMatchTable<int>>();
    for (int i = 0; i < arg.fields_size(); i++) {
      const auto &f = arg.fields(i);
      const auto err = fields->AddField(f.offset(), f.num_bytes(), 0, i);
      if (err.first) {
        return CommandFailure(-err.first, "Error adding field %d: %s", i,
                              err.second.c_str());
      }
    }
    config->mode = Mode::kOther;
    config->hasher = ExactMatchKeyHash(fields->total_key_size());
    config->fields_table = std::move(fields);
  } else if (arg.mode() == "l2") {
    config->mode = Mode::kL2;
  } else if (arg.mode() == "l3") {
    config->mode = Mode::kL3;
  } else if (arg.mode() == "l4") {
    config->mode = Mode::kL4;
  } else {
    return CommandFailure(EINVAL, "available LB modes: l2, l3, l4");
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
    ret = ApplyMode(mode_arg, &config);
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
        err = ApplyMode(mode_arg, &next);
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
  return bess::utils::Format("%zu fields",
                             config ? config->fields_table->num_fields() : 0);
}

template <>
inline void HashLB::DoProcessBatch<HashLB::Mode::kOther>(
    Context *ctx, bess::PacketBatch *batch, const Config &config) {
  void *bufs[bess::PacketBatch::kMaxBurst];
  ExactMatchKey keys[bess::PacketBatch::kMaxBurst];

  size_t cnt = batch->cnt();
  for (size_t i = 0; i < cnt; i++) {
    bufs[i] = batch->packet(i).head_data<void *>();
  }

  config.fields_table->MakeKeys((const void **)bufs, keys, cnt);
  const gate_idx_t *gates = config.gates.data();
  const uint16_t num_gates = static_cast<uint16_t>(config.gates.size());

  for (size_t i = 0; i < cnt; i++) {
    EmitPacket(ctx, batch->packet(i),
               gates[hash_range(config.hasher(keys[i]), num_gates)]);
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
  /* assumes untagged packets */
  const int ip_offset = sizeof(bess::utils::Ethernet);

  for (bess::PacketRef snb : batch->packets()) {
    if (unlikely(snb.head_len() <
                 sizeof(bess::utils::Ethernet) +
                     sizeof(bess::utils::Ipv4))) {
      // L3 hashes only the fixed src/dst words; options (a larger IHL) may
      // live in a later segment and do not move those offsets.
      EmitPacket(ctx, snb, DROP_GATE);
      continue;
    }
    char *head = snb.head_data<char *>();

    uint32_t hash_val;
    // Header fields need not be aligned (the IP header starts 14 bytes in).
    uint32_t v0 = Load<uint32_t>(head + ip_offset + 12);  /* src IP */
    v0 ^= Load<uint32_t>(head + ip_offset + 16);          /* dst IP */

    hash_val = hash_32(v0, 0);
    EmitPacket(ctx, snb, gates[hash_range(hash_val, num_gates)]);
  }
}

template <>
inline void HashLB::DoProcessBatch<HashLB::Mode::kL4>(
    Context *ctx, bess::PacketBatch *batch, const Config &config) {
  const gate_idx_t *gates = config.gates.data();
  const uint16_t num_gates = static_cast<uint16_t>(config.gates.size());
  /* assumes untagged packets */
  const int ip_offset = sizeof(bess::utils::Ethernet);

  for (bess::PacketRef snb : batch->packets()) {
    const size_t ihl = Ipv4HeaderLength(snb, 4);
    if (unlikely(ihl == 0)) {
      EmitPacket(ctx, snb, DROP_GATE);
      continue;
    }
    char *head = snb.head_data<char *>();
    const uint32_t l4_offset = ip_offset + ihl;
    uint32_t hash_val;
    uint32_t v0 = Load<uint32_t>(head + ip_offset + 12);  /* src IP */
    v0 ^= Load<uint32_t>(head + ip_offset + 16);          /* dst IP */
    v0 ^= Load<uint16_t>(head + l4_offset);               /* src port */
    v0 ^= Load<uint16_t>(head + l4_offset + 2);           /* dst port */

    v0 ^= *(reinterpret_cast<uint8_t *>(head + ip_offset + 9)); /* ip_proto */

    hash_val = hash_32(v0, 0);

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
      DoProcessBatch<Mode::kOther>(ctx, batch, config);
      break;
    default:
      DCHECK(0);
  }
}

ADD_MODULE(HashLB, "hash_lb",
           "splits packets on a flow basis with L2/L3/L4 header fields")
