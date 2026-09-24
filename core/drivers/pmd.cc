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

#include "pmd.h"

#include <algorithm>
#include <cstdint>

#include <rte_bus.h>
#include <rte_bus_pci.h>
#include <rte_ethdev.h>

#include "../packet_pool.h"
#include "../utils/ether.h"
#include "../utils/format.h"

PmdCapabilities PmdCapabilities::FromDeviceInfo(
    const rte_eth_dev_info &dev_info) {
  PmdCapabilities ret;
  ret.rx_scatter =
      (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_SCATTER) != 0;
  ret.min_mtu =
      dev_info.min_mtu != 0 ? dev_info.min_mtu : RTE_ETHER_MIN_MTU;
  ret.max_mtu = dev_info.max_mtu != 0 ? dev_info.max_mtu
                                     : RTE_ETHER_MAX_JUMBO_FRAME_LEN;
  if (dev_info.max_mtu != UINT16_MAX &&
      dev_info.max_rx_pktlen > dev_info.max_mtu) {
    ret.rx_frame_overhead = dev_info.max_rx_pktlen - dev_info.max_mtu;
  }
  ret.rx_offload_capa = dev_info.rx_offload_capa;
  ret.tx_offload_capa = dev_info.tx_offload_capa;
  ret.tx_queue_offload_capa = dev_info.tx_queue_offload_capa;
  ret.dev_capa = dev_info.dev_capa;
  ret.driver_name = dev_info.driver_name ? dev_info.driver_name : "";
  return ret;
}

bess::packet::TxOffloadCapabilities
PmdCapabilities::ToTxOffloadCapabilities(uint64_t device_tx_offloads,
                                         uint64_t queue_tx_offloads) const {
  const uint64_t device_enabled =
      tx_offload_capa & ~tx_queue_offload_capa & device_tx_offloads;
  const uint64_t queue_enabled =
      tx_offload_capa & tx_queue_offload_capa & queue_tx_offloads;
  const uint64_t effective = device_enabled | queue_enabled;
  const bool gtp_supported = driver_name == "net_ice" ||
                             driver_name == "net_i40e" ||
                             driver_name == "net_iavf";
  return {
      .checksums =
          {
              .ipv4_header =
                  (effective & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0,
              .udp = (effective & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) != 0,
              .tcp = (effective & RTE_ETH_TX_OFFLOAD_TCP_CKSUM) != 0,
              .outer_ipv4_header =
                  (effective & RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM) != 0,
              .outer_udp =
                  (effective & RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM) != 0,
          },
      .tunnel_encodings =
          {
              .generic_ip =
                  (device_enabled & RTE_ETH_TX_OFFLOAD_IP_TNL_TSO) != 0,
              .generic_udp =
                  (device_enabled & RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO) != 0,
              .gtp = gtp_supported,
          },
      .multi_segment_tx =
          (effective & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) != 0,
  };
}

uint64_t PmdCapabilities::ConfiguredTxOffloads() const {
  constexpr uint64_t kTxOffloads =
      RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
      RTE_ETH_TX_OFFLOAD_TCP_CKSUM | RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
      RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_MULTI_SEGS |
      RTE_ETH_TX_OFFLOAD_IP_TNL_TSO | RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO;
  return tx_offload_capa & ~tx_queue_offload_capa & kTxOffloads;
}


uint64_t PmdCapabilities::EffectiveQueueOffloads(
    bool introspection_succeeded, uint64_t queue_tx_offloads) const {
  if (!introspection_succeeded) {
    return 0;
  }
  return queue_tx_offloads & tx_queue_offload_capa;
}

size_t PmdCapabilities::RxFrameLengthFor(uint32_t mtu) const {
  return static_cast<size_t>(mtu) + rx_frame_overhead;
}

PmdCapabilities::RxMtuSupport PmdCapabilities::RxMtuSupportFor(
    uint32_t mtu, size_t usable_single_mbuf_bytes) const {
  if (mtu < min_mtu) {
    return RxMtuSupport::kBelowDeviceMinMtu;
  }
  if (mtu > max_mtu) {
    return RxMtuSupport::kExceedsDeviceMtu;
  }
  if (RxFrameLengthFor(mtu) <= usable_single_mbuf_bytes) {
    return RxMtuSupport::kSingleMbuf;
  }
  return rx_scatter ? RxMtuSupport::kScatter
                    : RxMtuSupport::kScatterUnsupported;
}

static size_t single_mbuf_rx_capacity(const bess::PacketPool &pool) {
  const size_t data_room = pool.mbuf_data_room_size();
  return data_room > RTE_PKTMBUF_HEADROOM
             ? data_room - RTE_PKTMBUF_HEADROOM
             : 0;
}

static const rte_eth_conf default_eth_conf(const rte_eth_dev_info &dev_info,
                                           int nb_rxq,
                                           bool enable_rx_scatter) {
  rte_eth_conf ret = {};

  ret.rxmode.mq_mode = (nb_rxq > 1) ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE;
  ret.rxmode.offloads = 0;
  ret.txmode.offloads =
      PmdCapabilities::FromDeviceInfo(dev_info).ConfiguredTxOffloads();
  if (enable_rx_scatter) {
    ret.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_SCATTER;
  }
  ret.rx_adv_conf.rss_conf = {
      .rss_key = nullptr,
      .rss_key_len = 0,
      .rss_hf = (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP |
                 RTE_ETH_RSS_SCTP) &
                dev_info.flow_type_rss_offloads,
      // rte_eth_rss_conf gained this field upstream; DEFAULT (0) preserves
      // prior behavior (drivers picked their own hash algorithm).
      .algorithm = RTE_ETH_HASH_FUNCTION_DEFAULT,
  };

  return ret;
}
CommandResponse PMDPort::ConfigureDevice(dpdk_port_t port_id,
                                          const rte_eth_dev_info &dev_info,
                                          bool enable_rx_scatter) {
  std::array<uint64_t, MAX_QUEUES_PER_DIR> configured_queue_offloads{};
  const int num_rxq = num_queues[PACKET_DIR_INC];
  const int num_txq = num_queues[PACKET_DIR_OUT];

  int sid = rte_eth_dev_socket_id(port_id);
  if (sid < 0 || sid > RTE_MAX_NUMA_NODES) {
    sid = 0;
  }

  bess::PacketPool *pool = bess::PacketPool::GetDefaultPool(sid);
  if (!pool) {
    return CommandFailure(ENODEV, "No default packet pool for socket %d", sid);
  }

  rte_eth_conf eth_conf =
      default_eth_conf(dev_info, num_rxq, enable_rx_scatter);
  eth_conf.lpbk_mode = loopback_ ? 1 : 0;

  int ret = rte_eth_dev_configure(port_id, num_rxq, num_txq, &eth_conf);
  if (ret != 0) {
    return CommandFailure(-ret, "rte_eth_dev_configure() failed");
  }
  const uint64_t configured_device_offloads = eth_conf.txmode.offloads;

  rte_eth_rxconf eth_rxconf = dev_info.default_rxconf;
  eth_rxconf.rx_drop_en = 1;

  for (int i = 0; i < num_rxq; i++) {
    ret = rte_eth_rx_queue_setup(port_id, i, queue_size[PACKET_DIR_INC], sid,
                                 &eth_rxconf, pool->pool());
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_rx_queue_setup() failed");
    }
  }

  for (int i = 0; i < num_txq; i++) {
    ret = rte_eth_tx_queue_setup(port_id, i, queue_size[PACKET_DIR_OUT], sid,
                                 nullptr);
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_tx_queue_setup() failed");
    }
    rte_eth_txq_info queue_info{};
    const int queue_info_ret = rte_eth_tx_queue_info_get(port_id, i,
                                                         &queue_info);
    configured_queue_offloads[i] = capabilities_.EffectiveQueueOffloads(
        queue_info_ret == 0,
        queue_info_ret == 0 ? queue_info.conf.offloads : 0);
    configured_queue_offloads[i] &= ~configured_device_offloads;
  }

  rte_eth_promiscuous_enable(port_id);
  if (vlan_offload_mask_) {
    ret = rte_eth_dev_set_vlan_offload(port_id, vlan_offload_mask_);
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_dev_set_vlan_offload() failed");
    }
  }

  tx_device_offloads_enabled_ = configured_device_offloads;
  tx_queue_offloads_enabled_ = configured_queue_offloads;
  rx_scatter_enabled_ = enable_rx_scatter;
  return CommandSuccess();
}

void PMDPort::InitDriver() {
  dpdk_port_t num_dpdk_ports = rte_eth_dev_count_avail();

  LOG(INFO) << static_cast<int>(num_dpdk_ports)
            << " DPDK PMD ports have been recognized:";

  for (dpdk_port_t i = 0; i < num_dpdk_ports; i++) {
    rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(i, &dev_info) != 0) {
      LOG(WARNING) << "rte_eth_dev_info_get(" << static_cast<int>(i)
                   << ") failed";
      continue;
    }

    bess::utils::Ethernet::Address lladdr;
    rte_eth_macaddr_get(i, reinterpret_cast<rte_ether_addr *>(lladdr.bytes));

    int numa_node = rte_eth_dev_socket_id(static_cast<int>(i));

    std::string pci_info;
    if (dev_info.device) {
      const rte_bus *bus = rte_bus_find_by_device(dev_info.device);
      if (bus && !strcmp(rte_bus_name(bus), "pci")) {
        // struct rte_pci_device is opaque in the public API now (only
        // exposed to driver-SDK builds), so vendor/device IDs aren't
        // reachable here anymore. The device's name string is already
        // the PCI address for PCI devices (e.g. "0000:03:00.0"), which
        // covers what this log line actually needs.
        pci_info = bess::utils::Format("%s  ", rte_dev_name(dev_info.device));
      }
    }

    LOG(INFO) << "DPDK port_id " << static_cast<int>(i) << " ("
              << dev_info.driver_name << ")   RXQ " << dev_info.max_rx_queues
              << " TXQ " << dev_info.max_tx_queues << "  " << lladdr.ToString()
              << "  " << pci_info << " numa_node " << numa_node;
  }
}

// Find a port attached to DPDK by its integral id.
// returns 0 and sets *ret_port_id to "port_id" if the port is valid and
// available.
// returns > 0 on error.
static CommandResponse find_dpdk_port_by_id(dpdk_port_t port_id,
                                            dpdk_port_t *ret_port_id) {
  if (port_id >= RTE_MAX_ETHPORTS) {
    return CommandFailure(EINVAL, "Invalid port id %d", port_id);
  }
  // rte_eth_devices[] is no longer part of the public API (struct rte_bus /
  // rte_pci_device are opaque now; direct port-state array indexing went
  // with it) -- rte_eth_dev_is_valid_port() is the current public
  // equivalent of the old ATTACHED check.
  if (!rte_eth_dev_is_valid_port(port_id)) {
    return CommandFailure(ENODEV, "Port id %d is not available", port_id);
  }

  *ret_port_id = port_id;
  return CommandSuccess();
}

// Find a port attached to DPDK by its PCI address.
// returns 0 and sets *ret_port_id to the port_id of the port at PCI address
// "pci" if it is valid and available. *ret_hot_plugged is set to true if the
// device was attached to DPDK as a result of calling this function.
// returns > 0 on error.
static CommandResponse find_dpdk_port_by_pci_addr(const std::string &pci,
                                                  dpdk_port_t *ret_port_id,
                                                  bool *ret_hot_plugged) {
  dpdk_port_t port_id = DPDK_PORT_UNKNOWN;

  if (pci.length() == 0) {
    return CommandFailure(EINVAL, "No PCI address specified");
  }

  rte_pci_addr addr;
  if (rte_pci_addr_parse(pci.c_str(), &addr) != 0) {
    return CommandFailure(EINVAL,
                          "PCI address must be like "
                          "dddd:bb:dd.ff or bb:dd.ff");
  }

  const rte_bus *bus = nullptr;

  // struct rte_pci_device is opaque in the public API now, so its ->addr
  // field isn't reachable for rte_pci_addr_cmp() here. Use DPDK's own
  // canonical PCI-name formatter (PCI_PRI_FMT: "%.4x:%.2x:%.2x.%x", NOT
  // "%08x:%02x:%02x.%02x" -- domain is 4 hex digits and function is
  // unpadded) and compare against rte_dev_name() instead, which returns
  char target_name[RTE_ETH_NAME_MAX_LEN];
  rte_pci_device_name(&addr, target_name, sizeof(target_name));

  dpdk_port_t num_dpdk_ports = rte_eth_dev_count_avail();
  for (dpdk_port_t i = 0; i < num_dpdk_ports; i++) {
    rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(i, &dev_info) != 0) {
      continue;
    }

    if (dev_info.device) {
      bus = rte_bus_find_by_device(dev_info.device);
      if (bus && !strcmp(rte_bus_name(bus), "pci")) {
        if (strcmp(target_name, rte_dev_name(dev_info.device)) == 0) {
          port_id = i;
          break;
        }
      }
    }
  }

  // If still not found, maybe the device has not been attached yet
  if (port_id == DPDK_PORT_UNKNOWN) {
    int ret;
    char name[RTE_ETH_NAME_MAX_LEN];
    // Use the same canonical formatter as target_name above.
    // rte_eth_dev_get_port_by_name() below does an exact string match
    // against the port's real (canonically-formatted) name, so this must
    // match precisely, not just be something rte_pci_addr_parse() accepts.
    rte_pci_device_name(&addr, name, sizeof(name));

    ret = rte_eal_hotplug_add("pci", name, "");
    if (ret < 0) {
      return CommandFailure(ENODEV, "Cannot attach PCI device %s", name);
    }
    ret = rte_eth_dev_get_port_by_name(name, &port_id);
    if (ret < 0) {
      return CommandFailure(ENODEV, "Cannot find port id for PCI device %s",
                            name);
    }
    *ret_hot_plugged = true;
  }

  *ret_port_id = port_id;
  return CommandSuccess();
}

// Find a DPDK vdev by name.
// returns 0 and sets *ret_port_id to the port_id of "vdev" if it is valid and
// available. *ret_hot_plugged is set to true if the device was attached to
// DPDK as a result of calling this function.
// returns > 0 on error.
static CommandResponse find_dpdk_vdev(const std::string &vdev,
                                      dpdk_port_t *ret_port_id,
                                      bool *ret_hot_plugged) {
  dpdk_port_t port_id = DPDK_PORT_UNKNOWN;

  if (vdev.length() == 0) {
    return CommandFailure(EINVAL, "No vdev specified");
  }

  int ret = rte_dev_probe(vdev.c_str());
  if (ret < 0) {
    return CommandFailure(ENODEV, "Cannot attach vdev %s", vdev.c_str());
  }

  rte_dev_iterator iterator;
  RTE_ETH_FOREACH_MATCHING_DEV(port_id, vdev.c_str(), &iterator) {
    LOG(INFO) << "port id: " << port_id << "matches vdev: " << vdev;
    rte_eth_iterator_cleanup(&iterator);
    break;
  }

  *ret_hot_plugged = true;
  *ret_port_id = port_id;
  return CommandSuccess();
}

CommandResponse PMDPort::Init(const bess::pb::PMDPortArg &arg) {
  dpdk_port_t ret_port_id = DPDK_PORT_UNKNOWN;
  rte_eth_dev_info dev_info;

  int num_txq = num_queues[PACKET_DIR_OUT];
  int num_rxq = num_queues[PACKET_DIR_INC];

  int ret;

  CommandResponse err;
  switch (arg.port_case()) {
    case bess::pb::PMDPortArg::kPortId: {
      err = find_dpdk_port_by_id(arg.port_id(), &ret_port_id);
      break;
    }
    case bess::pb::PMDPortArg::kPci: {
      err = find_dpdk_port_by_pci_addr(arg.pci(), &ret_port_id, &hot_plugged_);
      break;
    }
    case bess::pb::PMDPortArg::kVdev: {
      err = find_dpdk_vdev(arg.vdev(), &ret_port_id, &hot_plugged_);
      break;
    }
    default:
      return CommandFailure(EINVAL, "No port specified");
  }

  if (err.error().code() != 0) {
    return err;
  }

  if (ret_port_id == DPDK_PORT_UNKNOWN) {
    return CommandFailure(ENOENT, "Port not found");
  }

  /* Use defaut rx/tx configuration as provided by PMD drivers,
   * with minor tweaks */
  ret = rte_eth_dev_info_get(ret_port_id, &dev_info);
  if (ret != 0) {
    return CommandFailure(-ret, "rte_eth_dev_info_get() failed");
  }

  capabilities_ = PmdCapabilities::FromDeviceInfo(dev_info);
  loopback_ = arg.loopback();
  vlan_offload_mask_ =
      (arg.vlan_offload_rx_strip() ? RTE_ETH_VLAN_STRIP_OFFLOAD : 0) |
      (arg.vlan_offload_rx_filter() ? RTE_ETH_VLAN_FILTER_OFFLOAD : 0) |
      (arg.vlan_offload_rx_qinq() ? RTE_ETH_VLAN_EXTEND_OFFLOAD : 0);

  int sid = rte_eth_dev_socket_id(ret_port_id);
  if (sid < 0 || sid > RTE_MAX_NUMA_NODES) {
    sid = 0;  // if socket_id is invalid, set to 0
  }

  bess::PacketPool *pool = bess::PacketPool::GetDefaultPool(sid);
  if (!pool) {
    return CommandFailure(ENODEV, "No default packet pool for socket %d", sid);
  }

  const size_t usable_single_mbuf_bytes = single_mbuf_rx_capacity(*pool);
  const auto rx_mtu_support = capabilities_.RxMtuSupportFor(
      conf_.mtu, usable_single_mbuf_bytes);
  if (rx_mtu_support ==
      PmdCapabilities::RxMtuSupport::kBelowDeviceMinMtu) {
    return CommandFailure(EINVAL, "mtu %u is below PMD min_mtu %u", conf_.mtu,
                          capabilities_.min_mtu);
  }
  if (rx_mtu_support ==
      PmdCapabilities::RxMtuSupport::kExceedsDeviceMtu) {
    return CommandFailure(EINVAL, "mtu %u exceeds PMD max_mtu %u", conf_.mtu,
                          capabilities_.max_mtu);
  }
  if (rx_mtu_support ==
      PmdCapabilities::RxMtuSupport::kScatterUnsupported) {
    return CommandFailure(
        EINVAL,
        "mtu %u requires RX frame length %zu, exceeds usable single-mbuf "
        "RX capacity %zu and PMD does not support RX scatter",
        conf_.mtu, capabilities_.RxFrameLengthFor(conf_.mtu),
        usable_single_mbuf_bytes);
  }
  const bool enable_rx_scatter =
      rx_mtu_support == PmdCapabilities::RxMtuSupport::kScatter;

  // Let DPDK clamp the requested descriptor counts to each PMD's
  // min/max/align limits in one call. The old hand-rolled code here only
  // handled min/max and ignored nb_align, which several PMDs enforce (a
  // later queue_setup() failure rather than a silent adjustment).
  // queue_size is size_t but the ethdev API is uint16_t throughout
  // (queue_setup takes uint16_t too), so clamp to UINT16_MAX first to
  // avoid truncation on the narrowing conversion.
  uint16_t nb_rx_desc = static_cast<uint16_t>(
      std::min(queue_size[PACKET_DIR_INC], static_cast<size_t>(UINT16_MAX)));
  uint16_t nb_tx_desc = static_cast<uint16_t>(
      std::min(queue_size[PACKET_DIR_OUT], static_cast<size_t>(UINT16_MAX)));
  const uint16_t old_rx_desc = nb_rx_desc;
  const uint16_t old_tx_desc = nb_tx_desc;

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(ret_port_id, &nb_rx_desc, &nb_tx_desc);
  if (ret != 0) {
    return CommandFailure(-ret, "rte_eth_dev_adjust_nb_rx_tx_desc() failed");
  }

  if (nb_rx_desc != old_rx_desc) {
    LOG(WARNING) << "adjusting RX queue size from " << old_rx_desc << " to "
                 << nb_rx_desc;
  }
  if (nb_tx_desc != old_tx_desc) {
    LOG(WARNING) << "adjusting TX queue size from " << old_tx_desc << " to "
                 << nb_tx_desc;
  }
  queue_size[PACKET_DIR_INC] = nb_rx_desc;
  queue_size[PACKET_DIR_OUT] = nb_tx_desc;

  err = ConfigureDevice(ret_port_id, dev_info, enable_rx_scatter);
  if (err.error().code() != 0) {
    return err;
  }

  ret = rte_eth_dev_start(ret_port_id);
  if (ret != 0) {
    return CommandFailure(-ret, "rte_eth_dev_start() failed");
  }
  dpdk_port_id_ = ret_port_id;

  int numa_node = rte_eth_dev_socket_id(static_cast<int>(ret_port_id));
  node_placement_ =
      numa_node == -1 ? UNCONSTRAINED_SOCKET : (1ull << numa_node);

  rte_eth_macaddr_get(dpdk_port_id_,
                      reinterpret_cast<rte_ether_addr *>(conf_.mac_addr.bytes));

  // Reset hardware stat counters, as they may still contain previous data
  CollectStats(true);

  driver_ = dev_info.driver_name ?: "unknown";

  return CommandSuccess();
}

bool PMDPort::HasActiveOutputUsers() const {
  for (queue_t qid = 0; qid < num_queues[PACKET_DIR_OUT]; qid++) {
    if (users[PACKET_DIR_OUT][qid] != nullptr) {
      return true;
    }
  }
  return false;
}

CommandResponse PMDPort::UpdateConf(const Conf &conf) {
  const bool was_admin_up = conf_.admin_up;
  const bool old_rx_scatter_enabled = rx_scatter_enabled_;
  bool need_rx_reconfigure = false;
  bool rx_reconfigure_attempted = false;
  bool enable_rx_scatter = rx_scatter_enabled_;
  rte_eth_dev_info dev_info = {};

  // Validate all configuration that can be rejected before stopping the
  // device. A rejected request must not interrupt an otherwise running port.
  if (conf_.mtu != conf.mtu && conf.mtu != 0) {
    int sid = rte_eth_dev_socket_id(dpdk_port_id_);
    if (sid < 0 || sid > RTE_MAX_NUMA_NODES) {
      sid = 0;
    }
    bess::PacketPool *pool = bess::PacketPool::GetDefaultPool(sid);
    if (!pool) {
      return CommandFailure(ENODEV, "No default packet pool for socket %d", sid);
    }

    const size_t usable_single_mbuf_bytes = single_mbuf_rx_capacity(*pool);
    const auto rx_mtu_support = capabilities_.RxMtuSupportFor(
        conf.mtu, usable_single_mbuf_bytes);
    if (rx_mtu_support ==
        PmdCapabilities::RxMtuSupport::kBelowDeviceMinMtu) {
      return CommandFailure(EINVAL, "mtu %u is below PMD min_mtu %u", conf.mtu,
                            capabilities_.min_mtu);
    }
    if (rx_mtu_support ==
        PmdCapabilities::RxMtuSupport::kExceedsDeviceMtu) {
      return CommandFailure(EINVAL, "mtu %u exceeds PMD max_mtu %u", conf.mtu,
                            capabilities_.max_mtu);
    }
    if (rx_mtu_support ==
        PmdCapabilities::RxMtuSupport::kScatterUnsupported) {
      return CommandFailure(
          EINVAL,
          "mtu %u requires RX frame length %zu, exceeds usable single-mbuf "
          "RX capacity %zu and PMD does not support RX scatter",
          conf.mtu, capabilities_.RxFrameLengthFor(conf.mtu),
          usable_single_mbuf_bytes);
    }

    enable_rx_scatter =
        rx_mtu_support == PmdCapabilities::RxMtuSupport::kScatter;
    if (enable_rx_scatter != rx_scatter_enabled_) {
      if (HasActiveOutputUsers()) {
        return CommandFailure(
            EBUSY,
            "cannot change RX scatter while an output module owns a TX queue");
      }
      int ret = rte_eth_dev_info_get(dpdk_port_id_, &dev_info);
      if (ret != 0) {
        return CommandFailure(-ret, "rte_eth_dev_info_get() failed");
      }
      need_rx_reconfigure = true;
    }
  }

  int ret = rte_eth_dev_stop(dpdk_port_id_);
  if (ret != 0) {
    return CommandFailure(-ret, "rte_eth_dev_stop() failed");
  }

  CommandResponse resp = CommandSuccess();
  if (conf_.mtu != conf.mtu && conf.mtu != 0) {
    if (need_rx_reconfigure) {
      rx_reconfigure_attempted = true;
      resp = ConfigureDevice(dpdk_port_id_, dev_info, enable_rx_scatter);
      if (resp.error().code() != 0) {
        goto restart;
      }
    }

    ret = rte_eth_dev_set_mtu(dpdk_port_id_, conf.mtu);
    if (ret == 0) {
      conf_.mtu = conf.mtu;
    } else {
      resp = CommandFailure(-ret, "rte_eth_dev_set_mtu() failed");
      goto restart;
    }
  }

  if (conf_.mac_addr != conf.mac_addr && !conf.mac_addr.IsZero()) {
    rte_ether_addr tmp;
    rte_ether_addr_copy(
        reinterpret_cast<const rte_ether_addr *>(&conf.mac_addr.bytes), &tmp);
    ret = rte_eth_dev_default_mac_addr_set(dpdk_port_id_, &tmp);
    if (ret == 0) {
      conf_.mac_addr = conf.mac_addr;
    } else {
      resp = CommandFailure(-ret, "rte_eth_dev_default_mac_addr_set() failed");
      goto restart;
    }
  }

  if (conf.admin_up) {
    ret = rte_eth_dev_start(dpdk_port_id_);
    if (ret != 0) {
      resp = CommandFailure(-ret, "rte_eth_dev_start() failed");
      goto restart;
    }
    conf_.admin_up = true;
  } else {
    conf_.admin_up = false;
  }
  return resp;

restart:
  if (rx_reconfigure_attempted) {
    CommandResponse restore = ConfigureDevice(
        dpdk_port_id_, dev_info, old_rx_scatter_enabled);
    if (restore.error().code() != 0) {
      return CommandFailure(EIO,
                            "failed to restore RX configuration after update "
                            "failure");
    }
  }

  if (was_admin_up) {
    ret = rte_eth_dev_start(dpdk_port_id_);
    if (ret != 0) {
      conf_.admin_up = false;
      return CommandFailure(-ret, "rte_eth_dev_start() recovery failed");
    }
    conf_.admin_up = true;
  } else {
    conf_.admin_up = false;
  }
  return resp;
}

void PMDPort::DeInit() {
  rte_eth_dev_stop(dpdk_port_id_);

  if (hot_plugged_) {
    rte_eth_dev_info dev_info = {};
    if (rte_eth_dev_info_get(dpdk_port_id_, &dev_info) != 0) {
      LOG(WARNING) << "rte_eth_dev_info_get("
                   << static_cast<int>(dpdk_port_id_) << ") failed";
      // fall through: dev_info.device is left null, so the existing
      // "no device" handling below still runs rte_eth_dev_close().
    }

    char name[RTE_ETH_NAME_MAX_LEN];
    int ret;

    if (dev_info.device) {
      const rte_bus *bus = rte_bus_find_by_device(dev_info.device);
      if (rte_eth_dev_get_name_by_port(dpdk_port_id_, name) == 0) {
        rte_eth_dev_close(dpdk_port_id_);
        ret = rte_eal_hotplug_remove(rte_bus_name(bus), name);
        if (ret < 0) {
          LOG(WARNING) << "rte_eal_hotplug_remove("
                       << static_cast<int>(dpdk_port_id_)
                       << ") failed: " << rte_strerror(-ret);
        }
        return;
      } else {
        LOG(WARNING) << "rte_eth_dev_get_name failed for port"
                     << static_cast<int>(dpdk_port_id_);
      }
    } else {
      LOG(WARNING) << "rte_eth_def_info_get failed for port"
                   << static_cast<int>(dpdk_port_id_);
    }

    rte_eth_dev_close(dpdk_port_id_);
  }
}

void PMDPort::CollectStats(bool reset) {
  if (reset) {
    rte_eth_stats_reset(dpdk_port_id_);
    return;
  }

  rte_eth_stats stats;
  int ret = rte_eth_stats_get(dpdk_port_id_, &stats);
  if (ret < 0) {
    LOG(ERROR) << "rte_eth_stats_get(" << static_cast<int>(dpdk_port_id_)
               << ") failed: " << rte_strerror(-ret);
    return;
  }

  VLOG(1) << bess::utils::Format(
      "PMD port %d: ipackets %" PRIu64 " opackets %" PRIu64 " ibytes %" PRIu64
      " obytes %" PRIu64 " imissed %" PRIu64 " ierrors %" PRIu64
      " oerrors %" PRIu64 " rx_nombuf %" PRIu64,
      dpdk_port_id_, stats.ipackets, stats.opackets, stats.ibytes, stats.obytes,
      stats.imissed, stats.ierrors, stats.oerrors, stats.rx_nombuf);

  port_stats_.inc.dropped = stats.imissed;

  // NOTE:
  // - if link is down, tx bytes won't increase
  // - if destination MAC address is incorrect, rx pkts won't increase
  port_stats_.inc.packets = stats.ipackets;
  port_stats_.inc.bytes = stats.ibytes;
  port_stats_.out.packets = stats.opackets;
  port_stats_.out.bytes = stats.obytes;

  // Per-queue software stats (rte_eth_stats::q_ipackets/q_ibytes/q_errors/
  // q_opackets/q_obytes) were removed from DPDK's generic stats struct
  // upstream -- they were never supported by all PMDs to begin with (see
  // the driver exclusion list this replaced). Per-queue counters are still
  // available per-PMD through rte_eth_xstats_get() with driver-specific
  // named counters (e.g. "rx_q0_packets"), but that needs a real
  // xstats-name-to-id lookup and cache, not a mechanical field rename, so
  // it's left as follow-up work; queue_stats[][] simply stays at zero for
  // now, same as it already did for the drivers above.
}

int PMDPort::RecvPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) {
  return rte_eth_rx_burst(dpdk_port_id_, qid, pkts, cnt);
}

int PMDPort::SendPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) {
  int sent = rte_eth_tx_burst(dpdk_port_id_, qid, pkts, cnt);
  auto &stats = queue_stats[PACKET_DIR_OUT][qid];
  int dropped = cnt - sent;
  stats.dropped += dropped;
  stats.requested_hist[cnt]++;
  stats.actual_hist[sent]++;
  stats.diff_hist[dropped]++;
  return sent;
}

bess::packet::TxOffloadCapabilities PMDPort::GetTxOffloadCapabilities()
    const {
  const size_t queue_count = num_queues[PACKET_DIR_OUT];
  if (queue_count == 0 || queue_count > MAX_QUEUES_PER_DIR) {
    return {};
  }
  auto common = GetTxOffloadCapabilities(0);
  for (size_t index = 1; index < queue_count; index++) {
    const auto queue =
        GetTxOffloadCapabilities(static_cast<queue_t>(index));
    common.checksums.ipv4_header &= queue.checksums.ipv4_header;
    common.checksums.udp &= queue.checksums.udp;
    common.checksums.tcp &= queue.checksums.tcp;
    common.checksums.outer_ipv4_header &= queue.checksums.outer_ipv4_header;
    common.checksums.outer_udp &= queue.checksums.outer_udp;
    common.tunnel_encodings.generic_ip &= queue.tunnel_encodings.generic_ip;
    common.tunnel_encodings.generic_udp &= queue.tunnel_encodings.generic_udp;
    common.tunnel_encodings.gtp &= queue.tunnel_encodings.gtp;
    common.multi_segment_tx &= queue.multi_segment_tx;
  }
  return common;
}

bess::packet::TxOffloadCapabilities PMDPort::GetTxOffloadCapabilities(
    queue_t qid) const {
  if (qid >= num_queues[PACKET_DIR_OUT] || qid >= MAX_QUEUES_PER_DIR) {
    return {};
  }
  return capabilities_.ToTxOffloadCapabilities(
      tx_device_offloads_enabled_, tx_queue_offloads_enabled_[qid]);
}

Port::LinkStatus PMDPort::GetLinkStatus() {
  rte_eth_link status = {};
  // rte_eth_link_get() may block up to 9 seconds, so use _nowait() variant.
  int ret = rte_eth_link_get_nowait(dpdk_port_id_, &status);
  if (ret != 0) {
    LOG(WARNING) << "rte_eth_link_get_nowait("
                 << static_cast<int>(dpdk_port_id_)
                 << ") failed: " << rte_strerror(-ret);
  }

  return LinkStatus{.speed = status.link_speed,
                    .full_duplex = static_cast<bool>(status.link_duplex),
                    .autoneg = static_cast<bool>(status.link_autoneg),
                    .link_up = static_cast<bool>(status.link_status)};
}

ADD_DRIVER(PMDPort, "pmd_port", "DPDK poll mode driver")
