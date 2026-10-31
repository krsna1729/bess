// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "utils/logging.h"
#include <poll.h>
#include <signal.h>

#include <cerrno>
#include <cstring>

#include "unix_socket.h"

/*
 * Loop runner for accept calls.
 *
 * Note that all socket operations are run non-blocking, so that
 * the only place we block is in the ppoll() system call.
 */
void UnixSocketAcceptThread::Run() {
  struct pollfd fds[2];
  memset(fds, 0, sizeof(fds));
  fds[0].fd = owner_->listen_fd_;
  fds[0].events = POLLIN;
  fds[1].events = POLLRDHUP;

  while (true) {
    // negative FDs are ignored by ppoll()
    fds[1].fd = owner_->client_fd_.load(std::memory_order_relaxed);
    int res = ppoll(fds, 2, nullptr, Sigmask());

    if (IsExitRequested()) {
      return;

    } else if (res < 0) {
      if (errno == EINTR) {
        continue;
      } else {
        PLOG(ERROR) << "ppoll()";
      }

    } else if (fds[0].revents & POLLIN) {
      // new client connected
      int fd;
      while (true) {
        fd = accept4(owner_->listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd >= 0 || errno != EINTR) {
          break;
        }
      }
      if (fd < 0) {
        PLOG(ERROR) << "accept4()";
      } else if (owner_->client_fd_.load(std::memory_order_relaxed) !=
                 UnixSocketPort::kNotConnectedFd) {
        LOG(WARNING) << "Ignoring additional client\n";
        close(fd);
      } else {
        owner_->client_fd_.store(fd, std::memory_order_relaxed);
        if (owner_->confirm_connect_) {
          // Send confirmation that we've accepted their connect().
          send(fd, "yes", 4, 0);
        }
      }

    } else if (fds[1].revents & (POLLRDHUP | POLLHUP)) {
      // connection dropped by client
      int fd = owner_->client_fd_.load(std::memory_order_relaxed);
      owner_->client_fd_.store(UnixSocketPort::kNotConnectedFd,
                               std::memory_order_relaxed);
      close(fd);
    }
  }
}

// Refills the slots in [0, cnt) that RecvPackets consumed (left nullptr); a
// slot whose record was empty still holds its unused mbuf and is kept.
void UnixSocketPort::ReplenishRecvVector(int cnt) {
  DCHECK_LE(cnt, bess::PacketBatch::kMaxBurst);
  bess::PacketHandle fresh[bess::PacketBatch::kMaxBurst];
  int needed = 0;
  for (int i = 0; i < cnt; i++) {
    needed += (pkt_recv_vector_[i] == nullptr);
  }
  // On failure the slots stay holes (nullptr iov): their records are dropped.
  if (needed == 0 || !current_worker.packet_pool()->AllocBulk(fresh, needed)) {
    return;
  }

  int j = 0;
  for (int i = 0; i < cnt; i++) {
    if (pkt_recv_vector_[i] == nullptr) {
      pkt_recv_vector_[i] = fresh[j++];
      bess::PacketRef pkt(pkt_recv_vector_[i]);
      recv_iovecs_[i] = {.iov_base = pkt.head_data(),
                         .iov_len = pkt.tailroom()};
    }
  }
}

CommandResponse UnixSocketPort::Init(const bess::pb::UnixSocketPortArg &arg) {
  const std::string path = arg.path();
  int num_txq = num_queues[PACKET_DIR_OUT];
  int num_rxq = num_queues[PACKET_DIR_INC];

  int ret;

  if (num_txq > 1 || num_rxq > 1) {
    return CommandFailure(EINVAL, "Cannot have more than 1 queue per RX/TX");
  }

  if (arg.min_rx_interval_ns() < 0) {
    min_rx_interval_ns_ = 0;
  } else {
    min_rx_interval_ns_ = arg.min_rx_interval_ns() ?: kDefaultMinRxInterval;
  }

  confirm_connect_ = arg.confirm_connect();

  listen_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (listen_fd_ < 0) {
    DeInit();
    return CommandFailure(errno, "socket(AF_UNIX) failed");
  }

  addr_.sun_family = AF_UNIX;

  if (path.length() != 0) {
    snprintf(addr_.sun_path, sizeof(addr_.sun_path), "%s", path.c_str());
  } else {
    snprintf(addr_.sun_path, sizeof(addr_.sun_path), "%s/bess_unix_%s",
             P_tmpdir, name().c_str());
  }

  // This doesn't include the trailing null character.
  size_t addrlen = sizeof(addr_.sun_family) + strlen(addr_.sun_path);

  // Non-abstract socket address?
  if (addr_.sun_path[0] != '@') {
    // Remove existing socket file, if any.
    unlink(addr_.sun_path);
  } else {
    addr_.sun_path[0] = '\0';
  }

  ret = bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr_), addrlen);
  if (ret < 0) {
    DeInit();
    return CommandFailure(errno, "bind(%s) failed", addr_.sun_path);
  }

  ret = listen(listen_fd_, 1);
  if (ret < 0) {
    DeInit();
    return CommandFailure(errno, "listen() failed");
  }

  if (!accept_thread_.Start()) {
    DeInit();
    return CommandFailure(errno, "unable to start accept thread");
  }

  for (size_t i = 0; i < bess::PacketBatch::kMaxBurst; i++) {
    recv_vector_[i] = {.msg_hdr = {.msg_name = nullptr,
                                   .msg_namelen = 0,
                                   .msg_iov = &recv_iovecs_[i],
                                   .msg_iovlen = 1,
                                   .msg_control = nullptr,
                                   .msg_controllen = 0,
                                   .msg_flags = 0},
                       .msg_len = 0};
  }

  recv_iovecs_.fill({.iov_base = nullptr, .iov_len = 0});
  pkt_recv_vector_.fill(nullptr);
  ReplenishRecvVector(bess::PacketBatch::kMaxBurst);

  return CommandSuccess();
}

void UnixSocketPort::DeInit() {
  // End thread and wait for it (no-op if never started).
  accept_thread_.Terminate();

  if (listen_fd_ != kNotConnectedFd) {
    close(listen_fd_);
  }
  if (int fd = client_fd_.load(std::memory_order_relaxed);
      fd != kNotConnectedFd) {
    close(fd);
  }

  for (auto *pkt : pkt_recv_vector_) {
    bess::PacketFree(pkt);
  }
}

int UnixSocketPort::RecvPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) {
  int client_fd = client_fd_.load(std::memory_order_relaxed);

  DCHECK_EQ(qid, 0);

  if (client_fd == kNotConnectedFd) {
    last_idle_ns_ = 0;
    return 0;
  }

  uint64_t now_ns = current_worker.current_ns();
  if (now_ns - last_idle_ns_ < min_rx_interval_ns_) {
    return 0;
  }

  int received = 0;

  while (received < cnt) {
    int ret =
        recvmmsg(client_fd, recv_vector_.data(), cnt - received, 0, nullptr);

    if (ret > 0) {
      for (int i = 0; i < ret; i++) {
        if ((recv_iovecs_[i].iov_base != nullptr) &&
            (recv_vector_[i].msg_len > 0)) {
          bess::PacketRef pkt(pkt_recv_vector_[i]);
          pkt.append(recv_vector_[i].msg_len);
          pkts[received++] = pkt.handle();
          pkt_recv_vector_[i] = nullptr;
          recv_iovecs_[i] = {.iov_base = nullptr, .iov_len = 0};
        }
      }
      ReplenishRecvVector(ret);
    } else {
      break;
    }
  }

  last_idle_ns_ = (received == 0) ? now_ns : 0;

  return received;
}

int UnixSocketPort::SendPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) {
  int i;
  int sent = 0;
  int client_fd = client_fd_.load(std::memory_order_relaxed);

  DCHECK_EQ(qid, 0);

  if (client_fd == kNotConnectedFd) {
    return 0;
  }

  size_t iovec_idx = 0;
  for (i = 0; i < cnt; i++) {
    bess::PacketRef pkt(pkts[i]);
    int nb_segs = pkt.nb_segs();

    for (int j = 0; j < nb_segs; j++) {
      if (iovec_idx >= send_iovecs_.size()) {
        break;
      }
      send_iovecs_[iovec_idx++] = {
          .iov_base = pkt.head_data(),
          .iov_len = static_cast<size_t>(pkt.head_len())};
      pkt = pkt.next();
    }

    send_vector_[i] = {
        .msg_hdr = {.msg_name = nullptr,
                    .msg_namelen = 0,
                    .msg_iov = &send_iovecs_[iovec_idx - nb_segs],
                    .msg_iovlen = static_cast<size_t>(nb_segs),
                    .msg_control = nullptr,
                    .msg_controllen = 0,
                    .msg_flags = 0},
        .msg_len = 0};
  }

  if (!send_vector_.empty()) {
    sent = sendmmsg(client_fd, send_vector_.data(), i, 0);
    if (sent > 0) {
      bess::PacketFreeBulk(pkts, sent);
    } else {
      sent = 0;
    }
  }

  return sent;
}

ADD_DRIVER(UnixSocketPort, "unix_port",
           "packet exchange via a UNIX domain socket")
