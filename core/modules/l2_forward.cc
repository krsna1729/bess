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

#include "l2_forward.h"

#include "../dataplane/batch_stages.h"

#include <rte_hash_crc.h>

#include "../utils/endian.h"
#include "../utils/simd.h"

/******************************************************************************/
// TODO(barath): Move this test code elsewhere.

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void l2_forward_init_test() {
  int ret;
  struct l2_table l2tbl = {};

  ret = l2_init(&l2tbl, 0, 0);
  DCHECK_LT(ret, 0);

  ret = l2_init(&l2tbl, 4, 0);
  DCHECK_LT(ret, 0);

  ret = l2_init(&l2tbl, 0, 2);
  DCHECK_LT(ret, 0);

  ret = l2_init(&l2tbl, 4, 2);
  DCHECK(!ret);
  ret = l2_deinit(&l2tbl);
  DCHECK_EQ(ret, 0);

  ret = l2_init(&l2tbl, 4, 4);
  DCHECK(!ret);
  ret = l2_deinit(&l2tbl);
  DCHECK_EQ(ret, 0);

  ret = l2_init(&l2tbl, 4, 8);
  DCHECK_LT(ret, 0);

  ret = l2_init(&l2tbl, 6, 4);
  DCHECK_LT(ret, 0);

  ret = l2_init(&l2tbl, 2 << 10, 2);
  DCHECK_EQ(ret, 0);
  ret = l2_deinit(&l2tbl);
  DCHECK_EQ(ret, 0);

  ret = l2_init(&l2tbl, 2 << 10, 3);
  DCHECK_EQ(ret, 0);
}

static void l2_forward_entry_test() {
  int ret;
  struct l2_table l2tbl = {};

  uint64_t addr1 = 0x0123456701234567;
  uint64_t addr2 = 0x9876543210987654;
  uint16_t index1 = 0x0123;
  uint16_t gate_index = -1;

  ret = l2_init(&l2tbl, 4, 4);
  DCHECK_EQ(ret, 0);

  ret = l2_add_entry(&l2tbl, addr1, index1);
  LOG(INFO) << "add entry: " << addr1 << ", index: " << index1;
  DCHECK_EQ(ret, 0);

  ret = l2_find(&l2tbl, addr1, &gate_index);
  LOG(INFO) << "find entry: " << addr1 << ", index: " << gate_index;
  DCHECK_EQ(ret, 0);
  DCHECK_EQ(index1, gate_index);

  ret = l2_find(&l2tbl, addr2, &gate_index);
  DCHECK_LT(ret, 0);

  ret = l2_del_entry(&l2tbl, addr1);
  DCHECK_EQ(ret, 0);

  ret = l2_del_entry(&l2tbl, addr2);
  DCHECK_LT(ret, 0);

  ret = l2_find(&l2tbl, addr1, &gate_index);
  DCHECK_LT(ret, 0);

  ret = l2_deinit(&l2tbl);
  DCHECK_EQ(ret, 0);
}

static void l2_forward_flush_test() {
  int ret;
  struct l2_table l2tbl = {};

  uint64_t addr1 = 0x0123456701234567;
  uint16_t index1 = 0x0123;
  uint16_t gate_index;

  ret = l2_init(&l2tbl, 4, 4);
  DCHECK_EQ(ret, 0);

  ret = l2_add_entry(&l2tbl, addr1, index1);
  DCHECK_EQ(ret, 0);

  ret = l2_flush(&l2tbl);
  DCHECK_EQ(ret, 0);

  ret = l2_find(&l2tbl, addr1, &gate_index);
  DCHECK_LT(ret, 0);

  ret = l2_deinit(&l2tbl);
  DCHECK_EQ(ret, 0);
}

static void l2_forward_collision_test() {
  const int h_size = 4;
  const int b_size = 4;
  const int max_hb_cnt = h_size * b_size;

  int ret;
  int i;
  struct l2_table l2tbl = {};

  uint64_t addr[max_hb_cnt];
  uint16_t idx[max_hb_cnt];
  int success[max_hb_cnt];
  uint32_t offset;

  ret = l2_init(&l2tbl, h_size, b_size);
  DCHECK_EQ(ret, 0);

  /* collision happens */
  for (i = 0; i < max_hb_cnt; i++) {
    addr[i] = random() % ULONG_MAX;
    idx[i] = random() % USHRT_MAX;

    ret = l2_add_entry(&l2tbl, addr[i], idx[i]);
    LOG(INFO) << "insert result: " << addr[i] << " " << idx[i] << " " << ret;
    success[i] = (ret >= 0);
  }

  /* collision happens */
  for (i = 0; i < max_hb_cnt; i++) {
    uint16_t gate_index;
    gate_index = 0;
    offset = 0;

    ret = l2_find(&l2tbl, addr[i], &gate_index);

    LOG(INFO) << "find result: " << addr[i] << " " << gate_index << " "
              << offset;

    if (success[i]) {
      DCHECK_EQ(ret, 0);
      DCHECK_EQ(idx[i], gate_index);
    } else {
      DCHECK_NE(ret, 0);
    }
  }

  ret = l2_deinit(&l2tbl);
  DCHECK_EQ(ret, 0);
}

int test_all() {
  l2_forward_init_test();
  l2_forward_entry_test();
  l2_forward_flush_test();
  l2_forward_collision_test();

  return 0;
}

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

const Commands L2Forward::cmds = {
    {"add", "L2ForwardCommandAddArg", MODULE_CMD_FUNC(&L2Forward::CommandAdd),
     Command::THREAD_UNSAFE},
    {"delete", "L2ForwardCommandDeleteArg",
     MODULE_CMD_FUNC(&L2Forward::CommandDelete), Command::THREAD_UNSAFE},
    {"set_default_gate", "L2ForwardCommandSetDefaultGateArg",
     MODULE_CMD_FUNC(&L2Forward::CommandSetDefaultGate), Command::THREAD_SAFE},
    {"lookup", "L2ForwardCommandLookupArg",
     MODULE_CMD_FUNC(&L2Forward::CommandLookup), Command::THREAD_SAFE},
    {"populate", "L2ForwardCommandPopulateArg",
     MODULE_CMD_FUNC(&L2Forward::CommandPopulate), Command::THREAD_UNSAFE},
};

CommandResponse L2Forward::Init(const bess::pb::L2ForwardArg &arg) {
  int ret = 0;
  int size = arg.size();
  int bucket = arg.bucket();

  default_gate_ = DROP_GATE;

  if (size == 0) {
    size = DEFAULT_TABLE_SIZE;
  }
  if (bucket == 0) {
    bucket = MAX_BUCKET_SIZE;
  }

  ret = l2_init(&l2_table_, size, bucket);

  if (ret != 0) {
    return CommandFailure(-ret,
                          "initialization failed with argument "
                          "size: '%d' bucket: '%d'",
                          size, bucket);
  }

  // The table's size is fixed here, so the batch body is too (K4.6): a probe
  // branches on the loaded bucket (hit? primary or alternate?).
  lookup_body_ = bess::dataplane::ResolveLookupBody(
      bess::dataplane::LookupBody::kAuto,
      {.table_bytes = l2_table_bytes(&l2_table_),
       .dependent_lines = 1,
       .branches_on_loaded_data = true});

  return CommandSuccess();
}

void L2Forward::DeInit() {
  l2_deinit(&l2_table_);
}

void L2Forward::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  gate_idx_t default_gate = ACCESS_ONCE(default_gate_);

  const size_t cnt = batch->cnt();
  uint64_t dst[bess::PacketBatch::kMaxBurst];
  bess::dataplane::RunBatch(
      lookup_body_, cnt,
      [&](size_t i) {
        // destination MAC (first 6 bytes); assumes little endian
        dst[i] = *(batch->packet(i).head_data<uint64_t *>()) & 0x0000ffffffffffff;
        l2_prefetch(&l2_table_, dst[i]);
      },
      [&](size_t i) {
        gate_idx_t out_gate;
        const int ret = l2_find(&l2_table_, dst[i], &out_gate);
        EmitPacket(ctx, batch->packet(i), ret != 0 ? default_gate : out_gate);
      });
}

CommandResponse L2Forward::CommandAdd(
    const bess::pb::L2ForwardCommandAddArg &arg) {
  for (int i = 0; i < arg.entries_size(); i++) {
    const auto &entry = arg.entries(i);

    if (!entry.addr().length()) {
      return CommandFailure(EINVAL,
                            "add list item map must contain addr as a string");
    }

    const char *str_addr = entry.addr().c_str();
    int gate = entry.gate();
    char addr[6];

    if (parse_mac_addr(str_addr, addr) != 0) {
      return CommandFailure(EINVAL, "%s is not a proper mac address", str_addr);
    }

    int r = l2_add_entry(&l2_table_, l2_addr_to_u64(addr), gate);

    if (r == -EEXIST) {
      return CommandFailure(EEXIST, "MAC address '%s' already exist", str_addr);
    } else if (r == -ENOMEM) {
      return CommandFailure(ENOMEM, "Not enough space");
    } else if (r != 0) {
      return CommandFailure(-r);
    }
  }

  return CommandSuccess();
}

CommandResponse L2Forward::CommandDelete(
    const bess::pb::L2ForwardCommandDeleteArg &arg) {
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

    int r = l2_del_entry(&l2_table_, l2_addr_to_u64(addr));

    if (r == -ENOENT) {
      return CommandFailure(ENOENT, "MAC address '%s' does not exist",
                            str_addr);
    } else if (r != 0) {
      return CommandFailure(EINVAL, "Unknown Error: %d\n", r);
    }
  }

  return CommandSuccess();
}

CommandResponse L2Forward::CommandSetDefaultGate(
    const bess::pb::L2ForwardCommandSetDefaultGateArg &arg) {
  default_gate_ = arg.gate();
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

    gate_idx_t gate;
    int r = l2_find(&l2_table_, l2_addr_to_u64(addr), &gate);

    if (r == -ENOENT) {
      return CommandFailure(ENOENT, "MAC address '%s' does not exist",
                            str_addr);
    } else if (r != 0) {
      return CommandFailure(EINVAL, "Unknown Error: %d\n", r);
    }
    ret.add_gates(gate);
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

  base_u64 = l2_addr_to_u64(base_str);

  int cnt = arg.count();
  int gate_cnt = arg.gate_count();

  base_u64 = bess::utils::be64_t::swap(base_u64) >> 16;
  base_u64 = base_u64 >> 16;

  for (int i = 0; i < cnt; i++) {
    l2_add_entry(&l2_table_, bess::utils::be64_t::swap(base_u64 << 16),
                 i % gate_cnt);

    base_u64++;
  }

  return CommandSuccess();
}

ADD_MODULE(L2Forward, "l2_forward",
           "classifies packets with destination MAC address")
