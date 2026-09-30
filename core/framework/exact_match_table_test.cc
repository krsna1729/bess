// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "exact_match_table.h"

#include <gtest/gtest.h>

#include "../packet_pool.h"
#include "utils/endian.h"

using bess::framework::Error;
using bess::framework::ExactMatchField;
using bess::framework::ExactMatchKey;
using bess::framework::ExactMatchRuleFields;
using bess::framework::ExactMatchTable;

TEST(EmTableTest, AddField) {
  ExactMatchTable<uint8_t> em;
  Error err = em.AddField(0, 4, 0, 0);
  ASSERT_EQ(0, err.first);
  ASSERT_EQ(1, em.num_fields());
  ExactMatchField ret = em.get_field(0);
  EXPECT_EQ(0, ret.offset);
  EXPECT_EQ(4, ret.size);
  EXPECT_EQ(0xFFFFFFFF, ret.mask);
  err = em.AddField(0, 4, 0, MAX_FIELDS);
  ASSERT_EQ(EINVAL, err.first);
}

TEST(EmTableTest, AddRule) {
  ExactMatchTable<uint16_t> em;
  em.AddField(0, 4, 0, 0);
  ExactMatchRuleFields rule = {
      {0x01, 0x02, 0x03, 0x04},
  };
  Error err = em.AddRule(0xBEEF, rule);
  ASSERT_EQ(0, err.first);
}

TEST(EmTableTest, LookupOneFieldOneRule) {
  ExactMatchTable<uint16_t> em;
  em.AddField(0, 4, 0, 0);
  ExactMatchRuleFields rule = {
      {0x04, 0x03, 0x02, 0x01},
  };
  uint64_t buf = 0x01020304;
  uint64_t bad_buf = 0xBAD;
  ExactMatchKey key = em.MakeKey(&buf);
  ExactMatchKey bad_key = em.MakeKey(&bad_buf);
  em.AddRule(0xBEEF, rule);
  EXPECT_EQ(0xBEEF, em.Find(key, 0xDEAD));
  EXPECT_EQ(0xDEAD, em.Find(bad_key, 0xDEAD));
}

TEST(EmTableTest, LookupTwoFieldsOneRule) {
  ExactMatchTable<uint16_t> em;
  ASSERT_EQ(0, em.AddField(0, 4, 0, 0).first);
  ASSERT_EQ(0, em.AddField(6, 2, 0, 1).first);
  ASSERT_EQ(2, em.num_fields());
  ExactMatchRuleFields rule = {{0x04, 0x03, 0x02, 0x01}, {0x06, 0x05}};
  uint64_t buf = 0x0506000001020304;
  ExactMatchKey key = em.MakeKey(&buf);
  ASSERT_EQ(0, em.AddRule(0xBEEF, rule).first);
  uint16_t ret = em.Find(key, 0xDEAD);
  ASSERT_EQ(0xBEEF, ret);
}

TEST(EmTableTest, LookupTwoFieldsTwoRules) {
  ExactMatchTable<uint16_t> em;
  ASSERT_EQ(0, em.AddField(0, 4, 0, 0).first);
  ASSERT_EQ(0, em.AddField(6, 2, 0, 1).first);
  ASSERT_EQ(2, em.num_fields());
  ExactMatchRuleFields rule1 = {{0x04, 0x03, 0x02, 0x01}, {0x06, 0x05}};
  ExactMatchRuleFields rule2 = {{0x0F, 0x0E, 0x0D, 0x0C}, {0x06, 0x05}};
  uint64_t buf1 = 0x0506000001020304;
  uint64_t buf2 = 0x050600000C0D0E0F;
  uint64_t bad_buf = 0xBAD;
  const void *bufs[3] = {&buf1, &buf2, &bad_buf};
  ExactMatchKey keys[3];
  em.MakeKeys(bufs, keys, 3);
  ASSERT_EQ(0, em.AddRule(0xF00, rule1).first);
  ASSERT_EQ(0, em.AddRule(0xBA2, rule2).first);
  EXPECT_EQ(0xF00, em.Find(keys[0], 0xDEAD));
  EXPECT_EQ(0xBA2, em.Find(keys[1], 0xDEAD));
  EXPECT_EQ(0xDEAD, em.Find(keys[2], 0xDEAD));
}

// This test is for a specific bug introduced at one point
// where the MakeKeys function didn't clear out any random
// crud that might be on the stack.
TEST(EmTableTest, IgnoreBytesPastEnd) {
  ExactMatchTable<uint16_t> em;
  ASSERT_EQ(0, em.AddField(6, 1, 0, 0).first);
  ASSERT_EQ(0, em.AddField(7, 8, 0, 1).first);
  uint64_t buf[2] = {0x0102030405060708, 0x1112131415161718};
  const void *bufs[1] = {&buf};
  ExactMatchRuleFields rule = {
      {0x02}, {0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12}};
  ExactMatchKey keys[1];
  memset(keys, 0x55, sizeof(keys));
  em.MakeKeys(bufs, keys, 1);
  ASSERT_EQ(0, em.AddRule(0x600d, rule).first);
  uint16_t ret = em.Find(keys[0], 0xDEAD);
  ASSERT_EQ(0x600d, ret);
}

TEST(EmTableTest, FindMakeKeysPktBatch) {
  const size_t n = 2;
  ExactMatchTable<uint16_t> em;
  ExactMatchRuleFields rule = {{0x04, 0x03, 0x02, 0x01}};
  ExactMatchKey keys[n];
  bess::PacketBatch batch;
  bess::PlainPacketPool pool;
  bess::PacketHandle pkts[n];
  pool.AllocBulk(pkts, n, 0);
  char databuf[32] = {0};

  ASSERT_EQ(0, em.AddField(0, 4, 0, 0).first);
  ASSERT_EQ(0, em.AddRule(0xF00, rule).first);

  batch.clear();
  for (size_t i = 0; i < n; i++) {
    bess::PacketRef pkt(pkts[i]);
    bess::utils::Copy(pkt.append(sizeof(databuf)), databuf, sizeof(databuf));
    batch.add(pkt);
  }

  const auto buffer_fn = [](bess::PacketRef pkt, const ExactMatchField &) {
    return pkt.head_data<void *>();
  };
  em.MakeKeys(&batch, buffer_fn, keys);
  for (size_t i = 0; i < n; i++) {
    // Packets are bogus, shouldn't match anything.
    ASSERT_EQ(0xDEAD, em.Find(keys[i], 0xDEAD));
  }
}
