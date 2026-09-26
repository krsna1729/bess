// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "hash_lb.h"

#include <utility>
#include <vector>

static inline uint32_t hash_16(uint16_t val, uint32_t init_val) {
#if __x86_64
  return crc32c_sse42_u16(val, init_val);
#else
  return crc32c_2bytes(val, init_val);
#endif
}

static inline uint32_t hash_32(uint32_t val, uint32_t init_val) {
#if __x86_64
  return crc32c_sse42_u32(val, init_val);
#else
  return crc32c_1word(val, init_val);
#endif
}

/* Returns a value in [0, range) as a function of an opaque number.
 * Also see utils/random.h */
static inline uint16_t hash_range(uint32_t hashval, uint16_t range) {
#if 1
  union {
    uint64_t i;
    double d;
  } tmp;

  /* the resulting number is 1.(b0)(b1)..(b31)00000..00 */
  tmp.i = 0x3ff0000000000000ull | (static_cast<uint64_t>(hashval) << 20);

  return (tmp.d - 1.0) * range;
#else
  /* This IDIV instruction is significantly slower */
  return hashval % range;
#endif
}


const Commands HashLB::cmds = {
    {"set_mode", "HashLBCommandSetModeArg",
     MODULE_CMD_FUNC(&HashLB::CommandSetMode), Command::THREAD_SAFE},
    {"set_gates", "HashLBCommandSetGatesArg",
     MODULE_CMD_FUNC(&HashLB::CommandSetGates), Command::THREAD_SAFE}};

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
  bess::control::runtime().rcu().ReclaimReady();
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
  return CommandSuccess();
}

// Both commands build a whole new configuration and install it only if every
// part validated: a refused command changes nothing.
CommandResponse HashLB::CommandSetMode(
    const bess::pb::HashLBCommandSetModeArg &arg) {
  Config next = Current();
  CommandResponse ret = ApplyMode(arg, &next);
  if (ret.has_error()) {
    return ret;
  }
  Install(std::move(next));
  return CommandSuccess();
}

CommandResponse HashLB::CommandSetGates(
    const bess::pb::HashLBCommandSetGatesArg &arg) {
  Config next = Current();
  CommandResponse ret = ApplyGates(arg, &next);
  if (ret.has_error()) {
    return ret;
  }
  Install(std::move(next));
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
  }
  Install(std::move(config));
  return CommandSuccess();
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
  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef snb = batch->packet(i);
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
  const int ip_offset = 14;

  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef snb = batch->packet(i);
    char *head = snb.head_data<char *>();

    uint32_t hash_val;
    uint32_t v0 =
        *(reinterpret_cast<uint32_t *>(head + ip_offset + 12));   /* src IP */
    v0 ^= *(reinterpret_cast<uint32_t *>(head + ip_offset + 16)); /* dst IP */

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
  const int ip_offset = 14;

  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef snb = batch->packet(i);
    char *head = snb.head_data<char *>();
    uint32_t l4_offset =
        ip_offset + ((*(reinterpret_cast<uint8_t *>(head + ip_offset)) & 0x0F)
                     << 2); /* ip_offset + IHL */
    uint32_t hash_val;
    uint32_t v0 =
        *(reinterpret_cast<uint32_t *>(head + ip_offset + 12));   /* src IP */
    v0 ^= *(reinterpret_cast<uint32_t *>(head + ip_offset + 16)); /* dst IP*/
    v0 ^= *(reinterpret_cast<uint16_t *>(head + l4_offset));      /* src port */
    v0 ^= *(reinterpret_cast<uint16_t *>(head + l4_offset + 2));  /* dst port */

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
