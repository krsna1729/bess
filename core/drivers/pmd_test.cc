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

#include "pmd.h"

#include <gtest/gtest.h>

namespace {

rte_eth_dev_info MakeDeviceInfo(uint32_t max_mtu, bool rx_scatter) {
  rte_eth_dev_info info = {};
  info.max_mtu = max_mtu;
  info.rx_offload_capa =
      (rx_scatter ? RTE_ETH_RX_OFFLOAD_SCATTER : 0) | 0x400000000ULL;
  info.tx_offload_capa = 0x55;
  info.dev_capa = 0xaa;
  return info;
}

constexpr size_t kPayloadRoom = 2048;
constexpr size_t kSingleMbufCapacity = kPayloadRoom + RTE_PKTMBUF_HEADROOM;

using RxMtuSupport = PmdCapabilities::RxMtuSupport;

}  // namespace

TEST(PmdCapabilitiesTest, CopiesDeviceCapabilities) {
  const rte_eth_dev_info info = MakeDeviceInfo(9000, true);
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(info);

  EXPECT_TRUE(capabilities.rx_scatter);
  EXPECT_EQ(9000u, capabilities.max_mtu);
  EXPECT_EQ(info.rx_offload_capa, capabilities.rx_offload_capa);
  EXPECT_EQ(info.tx_offload_capa, capabilities.tx_offload_capa);
  EXPECT_EQ(info.dev_capa, capabilities.dev_capa);
}

TEST(PmdCapabilitiesTest, MtuFitsSingleMbufWithoutScatter) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(9000, true));

  EXPECT_EQ(RxMtuSupport::kSingleMbuf,
            capabilities.RxMtuSupportFor(1500, kSingleMbufCapacity));
}

TEST(PmdCapabilitiesTest, MtuExceedsRoomUsesScatter) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(9000, true));

  EXPECT_EQ(RxMtuSupport::kScatter,
            capabilities.RxMtuSupportFor(9000, kSingleMbufCapacity));
}

TEST(PmdCapabilitiesTest, MtuExceedsRoomWithoutScatterIsRejected) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(9000, false));

  EXPECT_EQ(RxMtuSupport::kScatterUnsupported,
            capabilities.RxMtuSupportFor(9000, kSingleMbufCapacity));
}

TEST(PmdCapabilitiesTest, MtuAboveDeviceMaximumIsRejected) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(1500, true));

  EXPECT_EQ(RxMtuSupport::kExceedsDeviceMtu,
            capabilities.RxMtuSupportFor(1501, kSingleMbufCapacity));
}

TEST(PmdCapabilitiesTest, JumboMbufRoomDoesNotNeedScatter) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(9000, true));

  EXPECT_EQ(RxMtuSupport::kSingleMbuf,
            capabilities.RxMtuSupportFor(9000, 9000));
}
