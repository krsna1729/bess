// SPDX-License-Identifier: BSD-3-Clause

// bessd's HTTP adapter (D-125) against a live daemon started with
// --http_addr: /metrics in the text exposition format (the check that
// covered tools/bess_prometheus.py, D-076), the JSON API over the Control
// service under the daemon_epoch rules, the event stream, errors, the page;
// and no HTTP listener without the flag.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "live_daemon.h"

namespace {

using bess::live::Daemon;
namespace v2 = bess::pb::v2;

struct HttpReply {
  int status = 0;
  std::string headers;  // the status line and headers
  std::string body;
};

int Connect(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ADD_FAILURE() << "connect to " << port << ": " << std::strerror(errno);
  }
  return fd;
}

// Reads until `done(buffer)` or EOF, at most 10 s.
std::string ReadUntil(int fd, bool (*done)(const std::string &)) {
  std::string buf;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done(buf)) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd p{fd, POLLIN, 0};
    if (left.count() <= 0 || poll(&p, 1, static_cast<int>(left.count())) <= 0) {
      ADD_FAILURE() << "no answer within 10 s; got: " << buf;
      break;
    }
    char chunk[4096];
    const ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      break;
    }
    buf.append(chunk, static_cast<size_t>(n));
  }
  return buf;
}

HttpReply Split(const std::string &raw) {
  HttpReply reply;
  const size_t end = raw.find("\r\n\r\n");
  if (raw.rfind("HTTP/1.1 ", 0) != 0 || end == std::string::npos) {
    ADD_FAILURE() << "not an HTTP answer: " << raw;
    return reply;
  }
  reply.status = std::stoi(raw.substr(9, 3));
  reply.headers = raw.substr(0, end + 2);
  reply.body = raw.substr(end + 4);
  return reply;
}

HttpReply Exchange(int port, const std::string &request) {
  const int fd = Connect(port);
  EXPECT_EQ(static_cast<ssize_t>(request.size()), write(fd, request.data(), request.size()));
  const std::string raw = ReadUntil(fd, [](const std::string &) { return false; });  // to EOF
  close(fd);
  return Split(raw);
}

HttpReply Get(int port, const std::string &path) {
  return Exchange(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
}

HttpReply Post(int port, const std::string &path, const std::string &body) {
  return Exchange(port, "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                        "application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body);
}

// A top-level JSON member's value: a string's contents or a bare token.
std::string JsonField(const std::string &json, const std::string &key) {
  const std::string quoted = "\"" + key + "\":";
  size_t at = json.find(quoted);
  if (at == std::string::npos) {
    return "";
  }
  at += quoted.size();
  if (json[at] == '"') {
    return json.substr(at + 1, json.find('"', at + 1) - at - 1);
  }
  return json.substr(at, json.find_first_of(",}", at) - at);
}

// The TCP ports process `pid` listens on (its sockets in /proc/net/tcp{,6}).
std::set<int> ListeningPorts(int pid) {
  const std::string proc = "/proc/" + std::to_string(pid);
  std::set<std::string> inodes;
  for (const auto &entry : std::filesystem::directory_iterator(proc + "/fd")) {
    std::error_code ec;
    const std::string target = std::filesystem::read_symlink(entry.path(), ec).string();
    if (!ec && target.rfind("socket:[", 0) == 0) {
      inodes.insert(target.substr(8, target.size() - 9));
    }
  }
  std::set<int> ports;
  for (const char *table : {"/net/tcp", "/net/tcp6"}) {
    std::ifstream in(proc + table);
    std::string line;
    std::getline(in, line);  // the header
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      std::string slot, local, remote, state, queues, timer, retransmits, uid, timeout, inode;
      fields >> slot >> local >> remote >> state >> queues >> timer >> retransmits >> uid >>
          timeout >> inode;
      if (state == "0A" && inodes.count(inode) != 0) {  // TCP_LISTEN
        ports.insert(std::stoi(local.substr(local.rfind(':') + 1), nullptr, 16));
      }
    }
  }
  return ports;
}

class HttpTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    http_port_ = bess::live::FreePort();
    daemon_ = Daemon::Launch({"--http_addr=127.0.0.1:" + std::to_string(http_port_)}).release();
  }
  static void TearDownTestSuite() {
    delete daemon_;
    daemon_ = nullptr;
  }
  void SetUp() override { daemon_->Reset(); }

  static std::string Epoch() {
    const HttpReply got = Post(http_port_, "/v2/GetPipeline", "{}");
    EXPECT_EQ(200, got.status) << got.body;
    return JsonField(got.body, "daemonEpoch");
  }

  static inline Daemon *daemon_ = nullptr;
  static inline int http_port_ = 0;
};

// The exposition format, as tools/bess_prometheus.py's output was checked
// (D-076): HELP then TYPE before a family's samples, each family once, in
// name order, of the kind ListMetrics declares; the daemon epoch last.
TEST_F(HttpTest, MetricsInTheTextFormat) {
  const HttpReply got = Get(http_port_, "/metrics");
  ASSERT_EQ(200, got.status) << got.body;
  EXPECT_NE(std::string::npos, got.headers.find("Content-Type: text/plain; version=0.0.4"));

  std::map<std::string, std::string> declared;  // family -> kind, from ListMetrics
  auto samples = daemon_->client().Metrics();
  ASSERT_TRUE(samples) << samples.error().message;
  for (const v2::MetricSample &s : *samples) {
    declared[s.name()] = s.kind() == v2::MetricSample::KIND_COUNTER ? "counter" : "gauge";
  }
  ASSERT_FALSE(declared.empty());

  std::map<std::string, std::string> typed;
  std::string helped, family, last_family;
  std::istringstream lines(got.body);
  std::string line;
  int sample_lines = 0;
  while (std::getline(lines, line)) {
    if (line.rfind("# HELP ", 0) == 0) {
      helped = line.substr(7, line.find(' ', 7) - 7);
      continue;
    }
    if (line.rfind("# TYPE ", 0) == 0) {
      std::istringstream f(line.substr(7));
      std::string kind;
      f >> family >> kind;
      EXPECT_EQ(helped, family) << "HELP right before TYPE";
      EXPECT_TRUE(typed.emplace(family, kind).second) << family << " typed twice";
      if (family != "bess_daemon_epoch") {  // last, after the sorted families
        EXPECT_LT(last_family, family) << "families in name order";
        last_family = family;
      }
      continue;
    }
    ASSERT_FALSE(line.empty() || line[0] == '#') << line;
    const std::string name = line.substr(0, line.find_first_of("{ "));
    EXPECT_EQ(family, name) << "a sample after its own family's TYPE: " << line;
    const std::string value = line.substr(line.rfind(' ') + 1);
    size_t used = 0;
    std::stod(value, &used);
    EXPECT_EQ(value.size(), used) << line;
    sample_lines++;
  }
  EXPECT_GT(sample_lines, 0);
  EXPECT_EQ("bess_daemon_epoch", family) << "the last family";
  for (const auto &[name, kind] : declared) {
    EXPECT_EQ(kind, typed[name]) << name;
  }
  EXPECT_EQ("counter", typed["bess_transactions_total"]);
  EXPECT_EQ("gauge", typed["bess_transaction_generation"]);
  EXPECT_EQ("gauge", typed["bess_daemon_epoch"]);
  EXPECT_NE(std::string::npos, got.body.find("\nbess_daemon_epoch " + Epoch() + "\n"));
}

// POST /v2/<Method> is the Control RPC, epoch rules included.
TEST_F(HttpTest, JsonApiCallsTheControlService) {
  const std::string epoch = Epoch();
  ASSERT_FALSE(epoch.empty());
  const std::string pipeline = R"({"pipeline": {"modules": [{"name": "s", "mclass": "Sink"}]})";

  // Mutating without the epoch: refused as over gRPC, with the typed detail.
  {
    const HttpReply got = Post(http_port_, "/v2/ApplyPipeline", pipeline + "}");
    EXPECT_EQ(400, got.status) << got.body;
    EXPECT_EQ("INVALID_ARGUMENT", JsonField(got.body, "status"));
    EXPECT_EQ("daemon_epoch", JsonField(got.body, "field")) << got.body;
  }
  {
    const HttpReply got = Post(http_port_, "/v2/ApplyPipeline",
                               pipeline + R"(, "daemonEpoch": ")" + epoch + "\"}");
    ASSERT_EQ(200, got.status) << got.body;
    EXPECT_EQ(epoch, JsonField(got.body, "daemonEpoch"));
    EXPECT_NE(std::string::npos, got.headers.find("Content-Type: application/json"));
  }
  {
    const HttpReply got = Post(http_port_, "/v2/GetPipeline", "");  // empty: {}
    ASSERT_EQ(200, got.status) << got.body;
    EXPECT_NE(std::string::npos, got.body.find(R"({"name":"s","mclass":"Sink")")) << got.body;
  }
  // The same daemon over gRPC sees it.
  auto via_grpc = daemon_->client().Pipeline();
  ASSERT_TRUE(via_grpc) << via_grpc.error().message;
  ASSERT_EQ(1, via_grpc->pipeline.modules_size());
  EXPECT_EQ("s", via_grpc->pipeline.modules(0).name());
}

TEST_F(HttpTest, EventsAsServerSentEvents) {
  const int fd = Connect(http_port_);
  const std::string request = "GET /v2/events HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_EQ(static_cast<ssize_t>(request.size()), write(fd, request.data(), request.size()));
  const std::string raw = ReadUntil(fd, [](const std::string &b) {
    const size_t head = b.find("\r\n\r\n");
    return head != std::string::npos && b.find("\n\n", head + 4) != std::string::npos;
  });
  close(fd);
  const HttpReply got = Split(raw);
  ASSERT_EQ(200, got.status) << raw;
  EXPECT_NE(std::string::npos, got.headers.find("Content-Type: text/event-stream"));
  // The first event says where the stream starts, under which epoch.
  const std::string first = got.body.substr(0, got.body.find("\n\n"));
  EXPECT_EQ(0u, first.rfind("id: " + Epoch() + ":", 0)) << first;
  const size_t data = first.find("\ndata: ");
  ASSERT_NE(std::string::npos, data) << first;
  const std::string json = first.substr(data + 7);
  EXPECT_EQ("bess.start", JsonField(json, "type")) << json;
  EXPECT_EQ(Epoch(), JsonField(json, "daemonEpoch")) << json;
}

TEST_F(HttpTest, Errors) {
  {
    const HttpReply got = Get(http_port_, "/no/such/path");
    EXPECT_EQ(404, got.status);
    EXPECT_EQ("NOT_FOUND", JsonField(got.body, "status")) << got.body;
  }
  EXPECT_EQ(404, Post(http_port_, "/v2/NoSuchMethod", "{}").status);
  EXPECT_EQ(404, Post(http_port_, "/v2/WatchEvents", "{}").status) << "streamed at /v2/events";
  {
    const HttpReply got = Get(http_port_, "/v2/GetPipeline");
    EXPECT_EQ(405, got.status);
    EXPECT_NE(std::string::npos, got.headers.find("Allow: POST"));
  }
  {
    const HttpReply got = Post(http_port_, "/v2/GetPipeline", "{not json");
    EXPECT_EQ(400, got.status);
    EXPECT_EQ("INVALID_ARGUMENT", JsonField(got.body, "status")) << got.body;
  }
  EXPECT_EQ(400, Post(http_port_, "/v2/GetPipeline", R"({"noSuchField": 1})").status);
  EXPECT_EQ(400, Get(http_port_, "/v2/events?bogus=1").status);
  // Still serving.
  EXPECT_EQ(200, Post(http_port_, "/v2/GetCapabilities", "{}").status);
}

TEST_F(HttpTest, StatusPage) {
  const HttpReply got = Get(http_port_, "/");
  ASSERT_EQ(200, got.status);
  EXPECT_NE(std::string::npos, got.headers.find("Content-Type: text/html"));
  for (const char *uses : {"\"/v2/\" + method", "\"GetPipeline\"", "\"ListMetrics\"",
                           "\"/v2/events\""}) {
    EXPECT_NE(std::string::npos, got.body.find(uses)) << uses;
  }
}

// Without --http_addr bessd listens on its gRPC port alone.
TEST(HttpDefaultTest, OffWithoutTheFlag) {
  Daemon &daemon = Daemon::Shared();
  ASSERT_TRUE(daemon.Alive());
  const int grpc_port = std::stoi(daemon.address().substr(daemon.address().rfind(':') + 1));
  EXPECT_EQ(std::set<int>{grpc_port}, ListeningPorts(daemon.pid()));
}

// The host is required: serving beyond loopback is spelled out.
TEST(HttpDefaultTest, AddressWithoutHostIsRefused) {
  for (const char *address : {":8080", "8080"}) {
    const auto ran = bess::live::RunProgram(
        {bess::live::Env("BESSD"), "-f", "-m", "0", "-p", std::to_string(bess::live::FreePort()),
         "-skip_root_check", "-dpdk=false", std::string("--http_addr=") + address},
        "", std::chrono::seconds(30));
    EXPECT_NE(0, ran.status) << address;
    EXPECT_NE(std::string::npos, ran.err.find("expected HOST:PORT")) << ran.err;
  }
}

}  // namespace
