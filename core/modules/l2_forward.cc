// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "l2_forward.h"

#include "dataplane/transaction_engine.h"
#include "utils/endian.h"

namespace {

constexpr int64_t kMaxTableSize = 1048576 * 64;
constexpr int kDefaultTableSize = 1024;
constexpr int kMaxBucketSize = 4;

// The table's key word for a MAC (domain 0): the six address bytes, first
// byte lowest, above 16 zero bits (l2::MakeKey's layout).
uint64_t MacKey(const char *addr) {
  uint64_t mac = 0;
  std::memcpy(&mac, addr, 6);
  return mac << 16;
}

// A table key word as a client spells the MAC: lower-case hex, two digits a
// byte, colon-separated ("aa:bb:cc:dd:ee:ff"; parse_mac_addr reads it back).
std::string MacString(uint64_t key) {
  const uint64_t mac = key >> 16;
  return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac & 0xff, mac >> 8 & 0xff,
                     mac >> 16 & 0xff, mac >> 24 & 0xff, mac >> 32 & 0xff, mac >> 40 & 0xff);
}

}  // namespace

static int parse_mac_addr(const char *str, char *addr) {
  if (str != nullptr && addr != nullptr) {
    int r = sscanf(str, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", addr, addr + 1,
                   addr + 2, addr + 3, addr + 4, addr + 5);

    if (r != 6) {
      return -EINVAL;
    }
  }

  return 0;
}

/******************************************************************************/

// LEGACY_ONLY (D-129): the entries are the "<name>/macs" resource and the
// default gate the "<name>/settings" singleton. lookup answers; populate is a
// test/bench generator.
const Commands L2Forward::cmds = {
    {"add", "L2ForwardCommandAddArg", MODULE_CMD_FUNC(&L2Forward::CommandAdd),
     Command::THREAD_SAFE, "", Command::LEGACY_ONLY},
    {"delete", "L2ForwardCommandDeleteArg",
     MODULE_CMD_FUNC(&L2Forward::CommandDelete), Command::THREAD_SAFE, "",
     Command::LEGACY_ONLY},
    {"set_default_gate", "L2ForwardCommandSetDefaultGateArg",
     MODULE_CMD_FUNC(&L2Forward::CommandSetDefaultGate), Command::THREAD_SAFE, "",
     Command::LEGACY_ONLY},
    {"lookup", "L2ForwardCommandLookupArg",
     MODULE_CMD_FUNC(&L2Forward::CommandLookup), Command::THREAD_SAFE,
     "L2ForwardCommandLookupResponse"},
    {"populate", "L2ForwardCommandPopulateArg",
     MODULE_CMD_FUNC(&L2Forward::CommandPopulate), Command::THREAD_SAFE, "",
     Command::ACTION, Command::EDITS},
};

const ResourceTables L2Forward::resource_tables = {
    ResourceTable::Map<bess::pb::L2ForwardMacKey, bess::pb::L2ForwardMacValue>("macs"),
    ResourceTable::Singleton<bess::pb::L2ForwardSettings>("settings"),
};

CommandResponse L2Forward::Init(const bess::pb::L2ForwardArg &arg) {
  // Wire values are int64: range-check before narrowing, or 2^32 + 1 would
  // become 1.
  if (arg.size() < 0 || arg.size() > kMaxTableSize || arg.bucket() < 0 ||
      arg.bucket() > kMaxBucketSize) {
    return CommandFailure(EINVAL,
                          "size must be in 0..%lld and bucket in 0..%d "
                          "(0: default)",
                          static_cast<long long>(kMaxTableSize), kMaxBucketSize);
  }
  int size = static_cast<int>(arg.size());
  int bucket = static_cast<int>(arg.bucket());

  if (arg.has_default_gate() && !bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(EINVAL, "invalid default_gate %llu",
                          static_cast<unsigned long long>(arg.default_gate()));
  }
  default_gate_.store(
      arg.has_default_gate() ? static_cast<gate_idx_t>(arg.default_gate()) : DROP_GATE,
      std::memory_order_relaxed);

  if (size == 0) {
    size = kDefaultTableSize;
  }
  if (bucket == 0) {
    bucket = kMaxBucketSize;
  }
  // The arguments keep their meaning, size * bucket entries (each a power of
  // two, size at least 2); the table lays them out itself.
  if (size < 2 || !std::has_single_bit(static_cast<unsigned>(size)) ||
      !std::has_single_bit(static_cast<unsigned>(bucket))) {
    return CommandFailure(EINVAL,
                          "initialization failed with argument "
                          "size: '%d' bucket: '%d'",
                          size, bucket);
  }
  table_ = Table::Create(static_cast<size_t>(size) * static_cast<size_t>(bucket));
  if (table_ == nullptr) {
    return CommandFailure(ENOMEM, "cannot allocate a table of %d entries", size * bucket);
  }

  // The entries as a transaction resource: a MAC that misses today is placed
  // at the default gate meanwhile, which is what a miss does here.
  resource_ = std::make_unique<bess::l2::MacTableResource<Table>>(
      name() + "/macs", *table_, [this] {
        return static_cast<uint16_t>(default_gate_.load(std::memory_order_relaxed) + 1);
      });
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::L2ForwardMacKey,
                                                   bess::pb::L2ForwardMacValue>>(
          [](const bess::pb::L2ForwardMacKey &key)
              -> std::expected<bess::dataplane::ResourceKey, std::string> {
            char addr[6];
            if (parse_mac_addr(key.addr().c_str(), addr) != 0) {
              return std::unexpected("invalid MAC address '" + key.addr() + "'");
            }
            return bess::l2::MacTableResource<Table>::Key(MacKey(addr));
          },
          [](const bess::pb::L2ForwardMacValue &value)
              -> std::expected<std::any, std::string> {
            if (!bess::IsValidGateValue(value.gate())) {
              return std::unexpected("invalid gate " + std::to_string(value.gate()));
            }
            return std::any(static_cast<uint16_t>(value.gate() + 1));
          },
          // The control thread is the table's only writer, so reading it
          // here (under the control-plane lock) races nothing.
          [this](const auto &visit) {
            table_->ForEach([this, &visit](uint32_t slot) {
              bess::pb::L2ForwardMacKey key;
              key.set_addr(MacString(table_->key(slot)));
              bess::pb::L2ForwardMacValue value;
              value.set_gate(table_->value(slot) - 1);
              visit(key, value);
            });
          }));
  if (auto registered = init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  // "<name>/settings": what set_default_gate sets. An atomic store, so
  // workers keep running.
  if (auto registered = settings_.Register(
          init_context().resources(), init_context().codecs(), name() + "/settings",
          [](const bess::pb::L2ForwardSettings &value)
              -> std::expected<gate_idx_t, std::string> {
            if (!bess::IsValidGateValue(value.default_gate())) {
              return std::unexpected("invalid default_gate " +
                                     std::to_string(value.default_gate()));
            }
            return static_cast<gate_idx_t>(value.default_gate());
          },
          [this](gate_idx_t &gate) { default_gate_.store(gate, std::memory_order_relaxed); },
          [this] {
            bess::pb::L2ForwardSettings value;
            value.set_default_gate(default_gate_.load(std::memory_order_relaxed));
            return value;
          },
          nullptr);
      !registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void L2Forward::DeInit() {
  settings_.Release();
  if (resource_ != nullptr) {
    // Cannot be refused: the entries reference nothing and nothing references
    // them, and this runs under the control-plane lock transactions take.
    auto unregistered = init_context().resources().Unregister(resource_->name());
    CHECK(unregistered) << unregistered.error();
    binding_.Reset();
    resource_.reset();
  }
  table_.reset();
}

void L2Forward::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  ClassifyBatch(batch, gates);
  EmitBatch(ctx, batch, gates);
}

void L2Forward::ClassifyBatch(const bess::PacketBatch *batch, gate_idx_t *gates) const {
  const gate_idx_t default_gate =
      default_gate_.load(std::memory_order_relaxed);

  const int cnt = batch->cnt();
  uint64_t keys[bess::PacketBatch::kMaxBurst];
  uint16_t values[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    // The destination MAC (first 6 bytes, little endian) as a key word: the
    // shift drops the two bytes past it.
    uint64_t head;
    std::memcpy(&head, batch->packet(i).head_data<const char *>(), sizeof(head));
    keys[i] = head << 16;
  }

  const uint64_t hits =
      table_->LookupBatch(std::span<const uint64_t>(keys, static_cast<size_t>(cnt)), values);
  for (int i = 0; i < cnt; i++) {
    gates[i] = (hits >> i) & 1 ? static_cast<gate_idx_t>(values[i] - 1) : default_gate;
  }
}

namespace {

// Gates are int64 on the wire: check before narrowing (65536 is not gate 0,
// -1 is not gate 65535). DROP_GATE (8192) fits the slot's 15-bit field.
bool ValidWireGate(int64_t gate) {
  return gate >= 0 && bess::IsValidGateValue(static_cast<uint64_t>(gate));
}

}  // namespace

// add/delete/populate change the table in place while workers keep reading
// (G1.2 mode C; one-word slots need no grace period). Each
// command validates everything first, and add undoes its earlier inserts if a
// later one fails for lack of space, so a refused command leaves the table as
// it was. That is command-level all-or-nothing, not dataplane atomicity:
// entries become visible to packets one by one as they are inserted, and a
// rolled-back add may have been seen briefly. All-or-nothing changes across
// tables are what G1.2b transactions add (referential, or scope-snapshot for a
// scope table: D-050); these commands promise neither.
CommandResponse L2Forward::CommandAdd(
    const bess::pb::L2ForwardCommandAddArg &arg) {
  std::vector<std::pair<uint64_t, gate_idx_t>> entries;
  entries.reserve(static_cast<size_t>(arg.entries_size()));
  std::unordered_set<uint64_t> seen;  // duplicates within the request
  seen.reserve(static_cast<size_t>(arg.entries_size()));
  for (int i = 0; i < arg.entries_size(); i++) {
    const auto &entry = arg.entries(i);
    if (!entry.addr().length()) {
      return CommandFailure(EINVAL,
                            "add list item map must contain addr as a string");
    }
    const char *str_addr = entry.addr().c_str();
    char addr[6];
    if (parse_mac_addr(str_addr, addr) != 0) {
      return CommandFailure(EINVAL, "%s is not a proper mac address", str_addr);
    }
    if (!ValidWireGate(entry.gate())) {
      return CommandFailure(EINVAL, "Invalid gate: %lld",
                            static_cast<long long>(entry.gate()));
    }
    const uint64_t mac = MacKey(addr);
    if (table_->Lookup(mac) != 0) {
      return CommandFailure(EEXIST, "MAC address '%s' already exist", str_addr);
    }
    if (!seen.insert(mac).second) {
      return CommandFailure(EEXIST, "MAC address '%s' given twice", str_addr);
    }
    entries.emplace_back(mac, static_cast<gate_idx_t>(entry.gate()));
  }

  for (size_t i = 0; i < entries.size(); i++) {
    if (table_->Insert(entries[i].first, static_cast<uint16_t>(entries[i].second + 1), 0) ==
        Table::kNotFound) {
      // Out of space part-way: take back what this command added.
      for (size_t j = 0; j < i; j++) {
        table_->Erase(table_->Find(entries[j].first));
      }
      return CommandFailure(ENOMEM, "Not enough space");
    }
  }
  return CommandSuccess();
}

CommandResponse L2Forward::CommandDelete(
    const bess::pb::L2ForwardCommandDeleteArg &arg) {
  std::vector<uint64_t> macs;
  macs.reserve(static_cast<size_t>(arg.addrs_size()));
  for (int i = 0; i < arg.addrs_size(); i++) {
    const auto &_addr = arg.addrs(i);
    if (!_addr.length()) {
      return CommandFailure(EINVAL, "lookup must be list of string");
    }
    const char *str_addr = _addr.c_str();
    char addr[6];
    if (parse_mac_addr(str_addr, addr) != 0) {
      return CommandFailure(EINVAL, "%s is not a proper mac address", str_addr);
    }
    const uint64_t mac = MacKey(addr);
    if (table_->Find(mac) == Table::kNotFound) {
      return CommandFailure(ENOENT, "MAC address '%s' does not exist",
                            str_addr);
    }
    macs.push_back(mac);
  }
  for (const uint64_t mac : macs) {
    const uint32_t slot = table_->Find(mac);
    if (slot != Table::kNotFound) {  // a duplicate in the request is gone already
      table_->Erase(slot);
    }
  }
  return CommandSuccess();
}

CommandResponse L2Forward::CommandSetDefaultGate(
    const bess::pb::L2ForwardCommandSetDefaultGateArg &arg) {
  if (!ValidWireGate(arg.gate())) {
    return CommandFailure(EINVAL, "Invalid gate: %lld",
                          static_cast<long long>(arg.gate()));
  }
  default_gate_.store(static_cast<gate_idx_t>(arg.gate()),
                      std::memory_order_relaxed);
  return CommandSuccess();
}

CommandResponse L2Forward::CommandLookup(
    const bess::pb::L2ForwardCommandLookupArg &arg) {
  bess::pb::L2ForwardCommandLookupResponse ret;
  for (int i = 0; i < arg.addrs_size(); i++) {
    const auto &_addr = arg.addrs(i);

    if (!_addr.length()) {
      return CommandFailure(EINVAL, "lookup must be list of string");
    }

    const char *str_addr = _addr.c_str();
    char addr[6];

    if (parse_mac_addr(str_addr, addr) != 0) {
      return CommandFailure(EINVAL, "%s is not a proper mac address", str_addr);
    }

    const uint32_t value = table_->Lookup(MacKey(addr));
    if (value == 0) {
      return CommandFailure(ENOENT, "MAC address '%s' does not exist",
                            str_addr);
    }
    ret.add_gates(value - 1);
  }

  return CommandSuccess(ret);
}

CommandResponse L2Forward::CommandPopulate(
    const bess::pb::L2ForwardCommandPopulateArg &arg) {
  const char *base;
  char base_str[6] = {0};
  uint64_t base_u64;

  if (!arg.base().length()) {
    return CommandFailure(EINVAL, "base must exist in gen, and must be string");
  }

  // parse base addr
  base = arg.base().c_str();
  if (parse_mac_addr(base, base_str) != 0) {
    return CommandFailure(EINVAL, "%s is not a proper mac address", base_str);
  }

  base_u64 = MacKey(base_str) >> 16;

  // gate_count 0 used to divide by zero (i % gate_cnt) and crash bessd.
  const int64_t capacity = static_cast<int64_t>(table_->capacity());
  if (arg.count() < 0 || arg.count() > capacity || arg.gate_count() <= 0 ||
      arg.gate_count() > MAX_GATES) {
    return CommandFailure(EINVAL,
                          "count must be in 0..%lld (the table's slots) and "
                          "gate_count in 1..%d",
                          static_cast<long long>(capacity), MAX_GATES);
  }
  const int64_t cnt = arg.count();
  const int64_t gate_cnt = arg.gate_count();

  // The MAC as a 48-bit number (aa:bb:cc:dd:ee:ff -> 0xaabbccddeeff). A
  // second `>> 16` here used to drop its two low bytes, so populate started
  // at 00:00:aa:bb:cc:dd (external audit, 2026-09-27).
  base_u64 = bess::utils::be64_t::swap(base_u64) >> 16;

  // Addresses already present are left as they are; the ones this command
  // adds are taken back if it runs out of space, like add.
  std::vector<uint64_t> added;
  added.reserve(static_cast<size_t>(cnt));
  for (int64_t i = 0; i < cnt; i++) {
    const uint64_t mac = bess::utils::be64_t::swap(base_u64 << 16) << 16;
    if (table_->Find(mac) == Table::kNotFound) {
      if (table_->Insert(mac, static_cast<uint16_t>(i % gate_cnt + 1), 0) != Table::kNotFound) {
        added.push_back(mac);
      } else {
        for (const uint64_t m : added) {
          table_->Erase(table_->Find(m));
        }
        return CommandFailure(ENOMEM, "Not enough space after %lld entries",
                              static_cast<long long>(i));
      }
    }
    base_u64++;
  }

  return CommandSuccess();
}

ADD_MODULE(L2Forward, "l2_forward",
           "classifies packets with destination MAC address")
