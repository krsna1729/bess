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

#include <utility>

namespace {

rte_eth_dev_info MakeDeviceInfo(uint16_t min_mtu, uint16_t max_mtu,
                                bool rx_scatter, uint32_t max_rx_pktlen) {
  rte_eth_dev_info info = {};
  info.min_mtu = min_mtu;
  info.max_mtu = max_mtu;
  info.max_rx_pktlen = max_rx_pktlen;
  info.rx_offload_capa =
      (rx_scatter ? RTE_ETH_RX_OFFLOAD_SCATTER : 0) | 0x400000000ULL;
  info.tx_offload_capa = 0x55;
  info.dev_capa = 0xaa;
  return info;
}

constexpr size_t kEtherOverhead = RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;
constexpr size_t kPayloadRoom = 2048;
constexpr size_t kMbufDataRoom = kPayloadRoom + RTE_PKTMBUF_HEADROOM;
constexpr size_t kUsableSingleMbufBytes =
    kMbufDataRoom - RTE_PKTMBUF_HEADROOM;

using RxMtuSupport = PmdCapabilities::RxMtuSupport;

}  // namespace

class PMDPortTestAccess {
 public:
  static void ConfigureDifferentQueueCapabilities(PMDPort &port) {
    rte_eth_dev_info info = {};
    info.driver_name = "net_ice";
    info.tx_offload_capa = RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                           RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                           RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
    info.tx_queue_offload_capa = RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                                 RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
    port.capabilities_ = PmdCapabilities::FromDeviceInfo(info);
    port.num_queues[PACKET_DIR_OUT] = 2;
    port.tx_device_offloads_enabled_ = RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
    port.tx_queue_offloads_enabled_[0] = RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
    port.tx_queue_offloads_enabled_[1] = RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
  }

  static bool RxScatterEnabled(const PMDPort &port) {
    return port.rx_scatter_enabled_;
  }

  static void SetRxScatterEnabled(PMDPort &port, bool enabled) {
    port.rx_scatter_enabled_ = enabled;
  }

  static void SetConf(PMDPort &port, const Port::Conf &conf) {
    port.conf_ = conf;
  }

  static Port::Conf GetConf(const PMDPort &port) { return port.conf_; }

  static bool ConfStateDegraded(const PMDPort &port) {
    return port.conf_state_degraded_;
  }

  static void SetConfStateDegraded(PMDPort &port, bool degraded) {
    port.conf_state_degraded_ = degraded;
  }

  static CommandResponse RunUpdateConf(
      PMDPort &port, const Port::Conf &conf, bool need_rx_reconfigure,
      bool enable_rx_scatter, std::function<int()> stop,
      std::function<int()> start, std::function<int(uint32_t)> set_mtu,
 std::function<int(rte_ether_addr *)> set_mac,
      std::function<CommandResponse(bool)> configure_rx) {
    PMDPort::UpdateConfOps ops{std::move(stop), std::move(start),
                               std::move(set_mtu), std::move(set_mac),
                               std::move(configure_rx)};
    return port.UpdateConfWithOps(conf, need_rx_reconfigure,
                                  enable_rx_scatter, ops);
  }
};

TEST(PMDPortUpdateConfTest, RollsBackMtuWhenMacUpdateFails) {
  PMDPort port;
  Port::Conf old_conf = PMDPortTestAccess::GetConf(port);
  old_conf.mtu = 1500;
  old_conf.admin_up = true;
  old_conf.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:01");
  PMDPortTestAccess::SetConf(port, old_conf);

  Port::Conf requested = old_conf;
  requested.mtu = 9000;
  requested.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:02");
  uint32_t hardware_mtu = old_conf.mtu;
  auto hardware_mac = old_conf.mac_addr;
  int stop_calls = 0;
  int start_calls = 0;
  int mac_calls = 0;

  const auto response = PMDPortTestAccess::RunUpdateConf(
      port, requested, false, false,
      [&] { ++stop_calls; return 0; },
      [&] { ++start_calls; return 0; },
      [&](uint32_t mtu) {
        hardware_mtu = mtu;
        return 0;
      },
      [&](rte_ether_addr *) {
        ++mac_calls;
        return mac_calls == 1 ? -EIO : 0;
      },
      [](bool) { return CommandSuccess(); });

  EXPECT_NE(0, response.error().code());
  EXPECT_EQ(1, stop_calls);
  EXPECT_EQ(1, start_calls);
  EXPECT_EQ(old_conf.mtu, hardware_mtu);
  EXPECT_EQ(old_conf.mac_addr, hardware_mac);
  EXPECT_EQ(old_conf.mtu, PMDPortTestAccess::GetConf(port).mtu);
  EXPECT_EQ(old_conf.mac_addr, PMDPortTestAccess::GetConf(port).mac_addr);
  EXPECT_EQ(old_conf.admin_up, PMDPortTestAccess::GetConf(port).admin_up);
}

TEST(PMDPortUpdateConfTest, RollsBackScatterAndConfigWhenStartFails) {
  PMDPort port;
  Port::Conf old_conf = PMDPortTestAccess::GetConf(port);
  old_conf.mtu = 1500;
  old_conf.admin_up = true;
  old_conf.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:01");
  PMDPortTestAccess::SetConf(port, old_conf);
  PMDPortTestAccess::SetRxScatterEnabled(port, false);

  Port::Conf requested = old_conf;
  requested.mtu = 9000;
  requested.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:02");
  uint32_t hardware_mtu = old_conf.mtu;
  auto hardware_mac = old_conf.mac_addr;
  bool hardware_scatter = false;
  int start_calls = 0;
  int configure_calls = 0;

  const auto response = PMDPortTestAccess::RunUpdateConf(
      port, requested, true, true,
      [] { return 0; },
      [&] {
        ++start_calls;
        return start_calls == 1 ? -EIO : 0;
      },
      [&](uint32_t mtu) {
        hardware_mtu = mtu;
        return 0;
      },
      [&](rte_ether_addr *mac) {
        hardware_mac = bess::utils::Ethernet::Address(mac->addr_bytes);
        return 0;
      },
      [&](bool enable) {
        ++configure_calls;
        hardware_scatter = enable;
        return CommandSuccess();
      });

  EXPECT_NE(0, response.error().code());
  EXPECT_EQ(2, start_calls);
  EXPECT_EQ(2, configure_calls);
  EXPECT_EQ(old_conf.mtu, hardware_mtu);
  EXPECT_EQ(old_conf.mac_addr, hardware_mac);
  EXPECT_FALSE(hardware_scatter);
  EXPECT_FALSE(PMDPortTestAccess::RxScatterEnabled(port));
  EXPECT_EQ(old_conf.mtu, PMDPortTestAccess::GetConf(port).mtu);
  EXPECT_EQ(old_conf.mac_addr, PMDPortTestAccess::GetConf(port).mac_addr);
  EXPECT_TRUE(PMDPortTestAccess::GetConf(port).admin_up);
}

TEST(PMDPortUpdateConfTest, TracksAppliedMtuWhenMtuRollbackFails) {
  PMDPort port;
  Port::Conf old_conf = PMDPortTestAccess::GetConf(port);
  old_conf.mtu = 1500;
  old_conf.admin_up = true;
  old_conf.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:01");
  PMDPortTestAccess::SetConf(port, old_conf);

  Port::Conf requested = old_conf;
  requested.mtu = 9000;
  requested.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:02");
  uint32_t hardware_mtu = old_conf.mtu;
  int set_mtu_calls = 0;
  int set_mac_calls = 0;

  const auto response = PMDPortTestAccess::RunUpdateConf(
      port, requested, false, false, [] { return 0; }, [] { return 0; },
      [&](uint32_t mtu) {
        ++set_mtu_calls;
        if (set_mtu_calls == 1) {
          hardware_mtu = mtu;
          return 0;
        }
        return -EIO;
      },
      [&](rte_ether_addr *) {
        return ++set_mac_calls == 1 ? -EIO : 0;
      },
      [](bool) { return CommandSuccess(); });

  EXPECT_EQ(EIO, response.error().code());
  EXPECT_EQ(9000u, hardware_mtu);
  EXPECT_EQ(9000u, PMDPortTestAccess::GetConf(port).mtu);
  EXPECT_FALSE(PMDPortTestAccess::GetConf(port).admin_up);
  EXPECT_TRUE(PMDPortTestAccess::ConfStateDegraded(port));
}

TEST(PMDPortUpdateConfTest, TracksAppliedMacWhenMacRollbackFails) {
  PMDPort port;
  Port::Conf old_conf = PMDPortTestAccess::GetConf(port);
  old_conf.admin_up = true;
  old_conf.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:01");
  PMDPortTestAccess::SetConf(port, old_conf);

  Port::Conf requested = old_conf;
  requested.mac_addr = bess::utils::Ethernet::Address("02:00:00:00:00:02");
  auto hardware_mac = old_conf.mac_addr;
  int set_mac_calls = 0;
  int start_calls = 0;

  const auto response = PMDPortTestAccess::RunUpdateConf(
      port, requested, false, false, [] { return 0; },
      [&] { return ++start_calls == 1 ? -EIO : 0; },
      [](uint32_t) { return 0; },
      [&](rte_ether_addr *mac) {
        ++set_mac_calls;
        if (set_mac_calls == 2) {
          return -EIO;
        }
        hardware_mac = bess::utils::Ethernet::Address(mac->addr_bytes);
        return 0;
      },
      [](bool) { return CommandSuccess(); });

  EXPECT_EQ(EIO, response.error().code());
  EXPECT_EQ(requested.mac_addr, hardware_mac);
  EXPECT_EQ(requested.mac_addr, PMDPortTestAccess::GetConf(port).mac_addr);
  EXPECT_FALSE(PMDPortTestAccess::GetConf(port).admin_up);
  EXPECT_TRUE(PMDPortTestAccess::ConfStateDegraded(port));
}

TEST(PMDPortUpdateConfTest, RejectsUpdatesWhenConfigurationIsDegraded) {
  PMDPort port;
  PMDPortTestAccess::SetConfStateDegraded(port, true);

  const auto response = port.UpdateConf(PMDPortTestAccess::GetConf(port));

  EXPECT_EQ(EIO, response.error().code());
}

TEST(PMDPortUpdateConfTest, RecoveryStartFailureMarksPortDown) {
  PMDPort port;
  Port::Conf old_conf = PMDPortTestAccess::GetConf(port);
  old_conf.admin_up = true;
  PMDPortTestAccess::SetConf(port, old_conf);
  int start_calls = 0;

  const auto response = PMDPortTestAccess::RunUpdateConf(
      port, old_conf, false, false,
      [] { return 0; },
      [&] {
        ++start_calls;
        return -EIO;
      },
      [](uint32_t) { return 0; },
      [](rte_ether_addr *) { return 0; },
      [](bool) { return CommandSuccess(); });

  EXPECT_NE(0, response.error().code());
  EXPECT_EQ(2, start_calls);
  EXPECT_FALSE(PMDPortTestAccess::GetConf(port).admin_up);
}

TEST(PmdCapabilitiesTest, CopiesDeviceCapabilitiesAndRxGeometry) {
  rte_eth_dev_info info = MakeDeviceInfo(576, 9000, true, 9018);
  info.tx_queue_offload_capa = 0x20;
  info.driver_name = "net_ice";
  const PmdCapabilities capabilities = PmdCapabilities::FromDeviceInfo(info);

  EXPECT_TRUE(capabilities.rx_scatter);
  EXPECT_EQ(576u, capabilities.min_mtu);
  EXPECT_EQ(9000u, capabilities.max_mtu);
  EXPECT_EQ(kEtherOverhead, capabilities.rx_frame_overhead);
  EXPECT_EQ(info.rx_offload_capa, capabilities.rx_offload_capa);
  EXPECT_EQ(info.tx_offload_capa, capabilities.tx_offload_capa);
  EXPECT_EQ(info.tx_queue_offload_capa, capabilities.tx_queue_offload_capa);
  EXPECT_EQ(info.dev_capa, capabilities.dev_capa);
  EXPECT_EQ("net_ice", capabilities.driver_name);
}

TEST(PmdCapabilitiesTest,
     SeparatesAdvertisedConfiguredAndEffectiveTxOffloads) {
  rte_eth_dev_info info = {};
  info.tx_offload_capa =
      RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
      RTE_ETH_TX_OFFLOAD_TCP_CKSUM | RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
      RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_MULTI_SEGS |
      RTE_ETH_TX_OFFLOAD_IP_TNL_TSO | RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO;
  const auto advertised = PmdCapabilities::FromDeviceInfo(info);
  const auto disabled = advertised.ToTxOffloadCapabilities(0, 0);
  EXPECT_FALSE(disabled.checksums.ipv4_header);
  EXPECT_FALSE(disabled.checksums.udp);
  EXPECT_FALSE(disabled.checksums.tcp);
  EXPECT_FALSE(disabled.checksums.outer_ipv4_header);
  EXPECT_FALSE(disabled.checksums.outer_udp);
  EXPECT_FALSE(disabled.tunnel_encodings.generic_ip);
  EXPECT_FALSE(disabled.tunnel_encodings.generic_udp);
  EXPECT_FALSE(disabled.multi_segment_tx);

  const uint64_t configured =
      RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
      RTE_ETH_TX_OFFLOAD_IP_TNL_TSO;
  const auto effective =
      advertised.ToTxOffloadCapabilities(configured, 0);
  EXPECT_TRUE(effective.checksums.ipv4_header);
  EXPECT_TRUE(effective.checksums.udp);
  EXPECT_FALSE(effective.checksums.tcp);
  EXPECT_FALSE(effective.checksums.outer_ipv4_header);
  EXPECT_FALSE(effective.checksums.outer_udp);
  EXPECT_TRUE(effective.tunnel_encodings.generic_ip);
  EXPECT_FALSE(effective.tunnel_encodings.generic_udp);
  EXPECT_FALSE(effective.multi_segment_tx);
}

TEST(PmdCapabilitiesTest, UnknownQueueConfigurationDisablesQueueOffloads) {
  rte_eth_dev_info info = {};
  info.tx_offload_capa = RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                         RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
  info.tx_queue_offload_capa = RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                               RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
  const auto capabilities = PmdCapabilities::FromDeviceInfo(info);

  EXPECT_EQ(0u, capabilities.EffectiveQueueOffloads(
                    false, info.tx_queue_offload_capa));
  EXPECT_EQ(info.tx_queue_offload_capa,
            capabilities.EffectiveQueueOffloads(
                true, info.tx_queue_offload_capa));
  EXPECT_EQ(RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
            capabilities.EffectiveQueueOffloads(
                true, RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                          RTE_ETH_TX_OFFLOAD_TCP_CKSUM));
}

TEST(PmdCapabilitiesTest, UsesConfiguredQueueDefaultsOnlyForAdvertisedQueueBits) {
  rte_eth_dev_info info = {};
  info.tx_offload_capa = RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                         RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                         RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
  info.tx_queue_offload_capa = RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                               RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
  const auto capabilities = PmdCapabilities::FromDeviceInfo(info);
  EXPECT_EQ(RTE_ETH_TX_OFFLOAD_TCP_CKSUM,
            capabilities.ConfiguredTxOffloads());

  const auto effective = capabilities.ToTxOffloadCapabilities(
      0,
      RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM);
  EXPECT_FALSE(effective.checksums.tcp);
  EXPECT_TRUE(effective.checksums.udp);
  EXPECT_TRUE(effective.checksums.outer_udp);
}
TEST(PMDPortCapabilitiesTest,
     PortUsesCommonCapabilitiesAndQueueOutUsesExactQueue) {
  PMDPort port;
  PMDPortTestAccess::ConfigureDifferentQueueCapabilities(port);

  const auto queue_zero = port.GetTxOffloadCapabilities(0);
  const auto queue_one = port.GetTxOffloadCapabilities(1);
  const auto common = port.GetTxOffloadCapabilities();
  EXPECT_TRUE(queue_zero.checksums.tcp);
  EXPECT_TRUE(queue_zero.checksums.udp);
  EXPECT_FALSE(queue_zero.checksums.outer_udp);
  EXPECT_TRUE(queue_one.checksums.tcp);
  EXPECT_FALSE(queue_one.checksums.udp);
  EXPECT_TRUE(queue_one.checksums.outer_udp);
  EXPECT_TRUE(common.checksums.tcp);
  EXPECT_FALSE(common.checksums.udp);
  EXPECT_FALSE(common.checksums.outer_udp);
  EXPECT_FALSE(port.GetTxOffloadCapabilities(2).checksums.tcp);
}


TEST(PMDPortCapabilitiesTest, DetectsActiveOutputUsers) {
  PMDPort port;
  port.num_queues[PACKET_DIR_OUT] = 2;
  EXPECT_FALSE(port.HasActiveOutputUsers());

  port.users[PACKET_DIR_OUT][1] = reinterpret_cast<const module *>(1);
  EXPECT_TRUE(port.HasActiveOutputUsers());
}

TEST(PmdCapabilitiesTest, MapsGtpOnlyForSourceVerifiedIntelDrivers) {
  rte_eth_dev_info info = {};
  info.tx_offload_capa = RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
  for (const char *driver : {"net_ice", "net_i40e", "net_iavf"}) {
    info.driver_name = driver;
    const auto effective =
        PmdCapabilities::FromDeviceInfo(info).ToTxOffloadCapabilities(
            RTE_ETH_TX_OFFLOAD_TCP_CKSUM, 0);
    EXPECT_TRUE(effective.tunnel_encodings.gtp);
    EXPECT_TRUE(effective.checksums.tcp);
  }
  info.driver_name = "net_unknown";
  const auto unknown =
      PmdCapabilities::FromDeviceInfo(info).ToTxOffloadCapabilities(
          RTE_ETH_TX_OFFLOAD_TCP_CKSUM, 0);
  EXPECT_FALSE(unknown.tunnel_encodings.gtp);
}

TEST(PmdCapabilitiesTest,
     UsesAdvertisedI40eOuterUdpBitOnlyForSupportedDeviceVariant) {
  rte_eth_dev_info info = {};
  info.driver_name = "net_i40e";
  info.tx_offload_capa = RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM;
  auto effective =
      PmdCapabilities::FromDeviceInfo(info).ToTxOffloadCapabilities(
          RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM, 0);
  EXPECT_TRUE(effective.checksums.outer_udp);

  info.tx_offload_capa = 0;
  effective = PmdCapabilities::FromDeviceInfo(info).ToTxOffloadCapabilities(
      RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM, 0);
  EXPECT_FALSE(effective.checksums.outer_udp);
}

TEST(PmdCapabilitiesTest, ConfiguresOnlySupportedChecksumAndGenericTunnelFlags) {
  rte_eth_dev_info info = {};
  info.tx_offload_capa =
      RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM |
      RTE_ETH_TX_OFFLOAD_IP_TNL_TSO | RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO |
      RTE_ETH_TX_OFFLOAD_TCP_TSO | RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO |
      RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  const auto capabilities = PmdCapabilities::FromDeviceInfo(info);
  EXPECT_EQ(RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM |
                RTE_ETH_TX_OFFLOAD_IP_TNL_TSO |
                RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO,
            capabilities.ConfiguredTxOffloads());
}

TEST(PmdCapabilitiesTest, MtuAtFrameCapacityFitsSingleMbuf) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(576, 9000, true, 9018));
  const uint32_t mtu = kUsableSingleMbufBytes - kEtherOverhead;

  EXPECT_EQ(kUsableSingleMbufBytes,
            capabilities.RxFrameLengthFor(mtu));
  EXPECT_EQ(RxMtuSupport::kSingleMbuf,
            capabilities.RxMtuSupportFor(mtu, kUsableSingleMbufBytes));
}

TEST(PmdCapabilitiesTest, MtuOneByteOverFrameCapacityUsesScatter) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(576, 9000, true, 9018));
  const uint32_t mtu = kUsableSingleMbufBytes - kEtherOverhead + 1;

  EXPECT_EQ(kUsableSingleMbufBytes + 1,
            capabilities.RxFrameLengthFor(mtu));
  EXPECT_EQ(RxMtuSupport::kScatter,
            capabilities.RxMtuSupportFor(mtu, kUsableSingleMbufBytes));
}

TEST(PmdCapabilitiesTest, MtuOverFrameCapacityWithoutScatterIsRejected) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(576, 9000, false, 9018));
  const uint32_t mtu = kUsableSingleMbufBytes - kEtherOverhead + 1;

  EXPECT_EQ(RxMtuSupport::kScatterUnsupported,
            capabilities.RxMtuSupportFor(mtu, kUsableSingleMbufBytes));
}

TEST(PmdCapabilitiesTest, MtuBelowDeviceMinimumIsRejected) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(576, 9000, true, 9018));

  EXPECT_EQ(RxMtuSupport::kBelowDeviceMinMtu,
            capabilities.RxMtuSupportFor(575, kUsableSingleMbufBytes));
}

TEST(PmdCapabilitiesTest, MtuAboveDeviceMaximumIsRejected) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(576, 1500, true, 1518));

  EXPECT_EQ(RxMtuSupport::kExceedsDeviceMtu,
            capabilities.RxMtuSupportFor(1501, kUsableSingleMbufBytes));
}

TEST(PmdCapabilitiesTest, JumboMbufRoomIncludesEthernetOverhead) {
  const PmdCapabilities capabilities =
      PmdCapabilities::FromDeviceInfo(MakeDeviceInfo(576, 9000, true, 9018));

  EXPECT_EQ(RxMtuSupport::kSingleMbuf,
            capabilities.RxMtuSupportFor(9000, 9000 + kEtherOverhead));
}

TEST(PmdCapabilitiesTest, UnknownDeviceOverheadUsesEthernetFallback) {
  const PmdCapabilities capabilities = PmdCapabilities::FromDeviceInfo(
      MakeDeviceInfo(576, UINT16_MAX, true, UINT32_MAX));

  EXPECT_EQ(kEtherOverhead, capabilities.rx_frame_overhead);
}
