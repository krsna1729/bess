// SPDX-License-Identifier: BSD-3-Clause

#include "unix_socket.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include "packet_pool.h"
#include "worker.h"

class UnixSocketPortTestAccess {
 public:
  // Inits `port` on an abstract address, then hands it one end of a
  // socketpair as its client; returns the other end.
  static int InitWithClient(UnixSocketPort &port, int64_t min_rx_interval_ns) {
    port.num_queues[PACKET_DIR_INC] = 1;
    port.num_queues[PACKET_DIR_OUT] = 1;
    bess::pb::UnixSocketPortArg arg;
    arg.set_path("@unix_socket_test");
    arg.set_min_rx_interval_ns(min_rx_interval_ns);
    EXPECT_EQ(0, port.Init(arg).error().code());
    int fds[2];
    EXPECT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds));
    port.client_fd_.store(fds[0], std::memory_order_relaxed);
    return fds[1];
  }
};

namespace {

bess::PacketPool *Pool() {
  static bess::PacketPool *pool = [] {
    bess::PacketPool::CreateDefaultPools(32767);
    current_worker.SetNonWorker();
    return current_worker.packet_pool();
  }();
  return pool;
}

// RT-2: an empty record keeps its slot's mbuf; only the delivered packet
// leaves the pool.
TEST(UnixSocketPortTest, EmptyRecordsDoNotLeakMbufs) {
  bess::PacketPool *pool = Pool();
  UnixSocketPort port;
  int peer = UnixSocketPortTestAccess::InitWithClient(port, -1);
  size_t before = pool->Size();
  bess::PacketHandle pkts[bess::PacketBatch::kMaxBurst];
  int received = 0;
  // 10 rounds of 100 empty records, each drained so the socket never fills.
  for (int round = 0; round < 10; round++) {
    for (int i = 0; i < 100; i++) {
      ASSERT_EQ(0, send(peer, "", 0, 0));
    }
    received += port.RecvPackets(0, pkts + received, 1);
  }
  const char payload[] = "x";
  ASSERT_EQ(1, send(peer, payload, 1, 0));
  received += port.RecvPackets(0, pkts + received, 1);
  size_t after = pool->Size();

  EXPECT_EQ(1, received);
  EXPECT_EQ(1u, before - after);
  bess::PacketFreeBulk(pkts, received);
  close(peer);
  port.DeInit();
}

// RT-3: the idle throttle measures current_ns, not the TSC. While the TSC
// races ahead, a poll within min_rx_interval_ns of an idle one is skipped.
TEST(UnixSocketPortTest, IdleThrottleUsesNanoseconds) {
  Pool();
  UnixSocketPort port;
  int peer = UnixSocketPortTestAccess::InitWithClient(port, 1000);
  bess::PacketHandle pkt;

  current_worker.set_current_tsc(1'000'000'000);
  current_worker.set_current_ns(5000);
  EXPECT_EQ(0, port.RecvPackets(0, &pkt, 1));  // idle

  const char payload[] = "x";
  ASSERT_EQ(1, send(peer, payload, 1, 0));

  int polled = 0;
  for (int i = 1; i <= 10; i++) {  // 10 polls within the interval
    current_worker.set_current_tsc(1'000'000'000ull * (i + 1));
    current_worker.set_current_ns(5000 + i * 10);
    polled += port.RecvPackets(0, &pkt, 1);
  }
  EXPECT_EQ(0, polled);

  current_worker.set_current_ns(5000 + 1000);
  ASSERT_EQ(1, port.RecvPackets(0, &pkt, 1));
  bess::PacketFree(pkt);
  close(peer);
  port.DeInit();
}

}  // namespace
