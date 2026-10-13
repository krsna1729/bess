// SPDX-License-Identifier: BSD-3-Clause

#include "control/http_server.h"

#include <arpa/inet.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/generic/generic_stub.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <future>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "control/http_ui.h"
#include "control/metrics_exposition.h"
#include "pb/control_v2.grpc.pb.h"
#include "utils/common.h"
#include "utils/http_parser.h"
#include "utils/logging.h"

namespace bess::control {
namespace {

namespace v2 = pb::v2;

constexpr size_t kMaxHeaderBytes = 64 << 10;
// Bodies up to the gRPC server's message limit (kMaxMessageBytes).
constexpr size_t kMaxBodyBytes = 64 << 20;
constexpr size_t kMaxHeaders = 64;
// Each connection is a thread; past this many, 503 at once.
constexpr size_t kMaxConnections = 64;
// A request must arrive within this of its connection's accept, and each
// reply (each event, on a stream) be taken within this of its first byte.
constexpr std::chrono::seconds kIoTimeout{10};
// How often an event stream checks whether its client left.
constexpr int kWatchIntervalMs = 200;

// -- addresses ----------------------------------------------------------------

std::string FormatAddress(const sockaddr *sa, socklen_t len) {
  char host[NI_MAXHOST], serv[NI_MAXSERV];
  if (getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "?";
  }
  if (sa->sa_family == AF_INET6) {
    return std::string("[") + host + "]:" + serv;
  }
  return std::string(host) + ":" + serv;
}

bool IsLoopback(const sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    const auto *in = reinterpret_cast<const sockaddr_in *>(sa);
    return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
  }
  if (sa->sa_family == AF_INET6) {
    const in6_addr &a = reinterpret_cast<const sockaddr_in6 *>(sa)->sin6_addr;
    if (IN6_IS_ADDR_LOOPBACK(&a)) {
      return true;
    }
    return IN6_IS_ADDR_V4MAPPED(&a) && a.s6_addr[12] == 127;
  }
  return false;
}

// -- HTTP ---------------------------------------------------------------------

struct Request {
  std::string method;
  std::string path;   // without the query
  std::string query;  // after '?', undecoded
  std::optional<std::string> host;
  std::optional<std::string> origin;
  std::string fetch_site;  // Sec-Fetch-Site
  std::string content_type;
  std::string last_event_id;
  std::string body;
};

struct Reply {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::string allow;  // for 405
};

const char *Reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 499: return "Client Closed Request";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Error";
  }
}

using Deadline = std::chrono::steady_clock::time_point;

// Waits until `fd` is ready for `events`; false when `deadline` passes first.
bool WaitFor(int fd, short events, Deadline deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return false;
    }
    pollfd p{fd, events, 0};
    const int n = poll(&p, 1, static_cast<int>(left.count()));
    if (n > 0) {
      return true;
    }
    if (n < 0 && errno != EINTR) {
      return false;
    }
  }
}

// All of `data` by `deadline`: a client that takes it a byte at a time
// cannot hold the connection past it.
bool SendAll(int fd, std::string_view data, Deadline deadline) {
  while (!data.empty()) {
    if (!WaitFor(fd, POLLOUT, deadline)) {
      return false;
    }
    const ssize_t n = send(fd, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

void SendReply(int fd, const Reply &reply) {
  std::string head = "HTTP/1.1 " + std::to_string(reply.status) + " " + Reason(reply.status) +
                     "\r\nContent-Type: " + reply.content_type +
                     "\r\nContent-Length: " + std::to_string(reply.body.size()) +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\n";
  if (!reply.allow.empty()) {
    head += "Allow: " + reply.allow + "\r\n";
  }
  head += "\r\n";
  const Deadline deadline = std::chrono::steady_clock::now() + kIoTimeout;
  if (SendAll(fd, head, deadline)) {
    SendAll(fd, reply.body, deadline);
  }
}

void AppendJsonString(std::string &out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0xf];
          out += kHex[c & 0xf];
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// grpc::StatusCode's names, by value (0-16).
const char *CodeName(grpc::StatusCode code) {
  static constexpr const char *kNames[] = {
      "OK",                 "CANCELLED",     "UNKNOWN",           "INVALID_ARGUMENT",
      "DEADLINE_EXCEEDED",  "NOT_FOUND",     "ALREADY_EXISTS",    "PERMISSION_DENIED",
      "RESOURCE_EXHAUSTED", "FAILED_PRECONDITION", "ABORTED",     "OUT_OF_RANGE",
      "UNIMPLEMENTED",      "INTERNAL",      "UNAVAILABLE",       "DATA_LOSS",
      "UNAUTHENTICATED"};
  const auto i = static_cast<size_t>(code);
  return i < std::size(kNames) ? kNames[i] : "UNKNOWN";
}

// google.rpc.Code's HTTP mapping (as gRPC-HTTP gateways answer).
int HttpStatus(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK: return 200;
    case grpc::StatusCode::CANCELLED: return 499;
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::FAILED_PRECONDITION:
    case grpc::StatusCode::OUT_OF_RANGE: return 400;
    case grpc::StatusCode::DEADLINE_EXCEEDED: return 504;
    case grpc::StatusCode::NOT_FOUND: return 404;
    case grpc::StatusCode::ALREADY_EXISTS:
    case grpc::StatusCode::ABORTED: return 409;
    case grpc::StatusCode::PERMISSION_DENIED: return 403;
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return 429;
    case grpc::StatusCode::UNIMPLEMENTED: return 501;
    case grpc::StatusCode::UNAVAILABLE: return 503;
    case grpc::StatusCode::UNAUTHENTICATED: return 401;
    default: return 500;
  }
}

// {"code": N, "status": "NAME", "message": "...", "detail": {...}}.
Reply ErrorReply(int http_status, grpc::StatusCode code, std::string_view message,
                 const std::string &detail_json = "") {
  Reply reply;
  reply.status = http_status;
  reply.body = "{\"code\":" + std::to_string(static_cast<int>(code)) + ",\"status\":";
  AppendJsonString(reply.body, CodeName(code));
  reply.body += ",\"message\":";
  AppendJsonString(reply.body, message);
  if (!detail_json.empty()) {
    reply.body += ",\"detail\":" + detail_json;
  }
  reply.body += "}\n";
  return reply;
}

Reply StatusReply(const grpc::Status &status, const grpc::ClientContext &ctx) {
  std::string detail_json;
  const auto &trailers = ctx.GetServerTrailingMetadata();
  if (const auto it = trailers.find("bess-error-bin"); it != trailers.end()) {
    v2::ErrorDetail detail;
    if (detail.ParseFromArray(it->second.data(), static_cast<int>(it->second.size()))) {
      (void)google::protobuf::util::MessageToJsonString(detail, &detail_json);
    }
  }
  return ErrorReply(HttpStatus(status.error_code()), status.error_code(), status.error_message(),
                    detail_json);
}

// Up to `size` bytes by `deadline`: > 0 read, 0 when the client closed
// (or failed), -1 when the deadline passed (`error` is then a 408).
ssize_t RecvBy(int fd, char *buf, size_t size, Deadline deadline, Reply *error) {
  for (;;) {
    if (!WaitFor(fd, POLLIN, deadline)) {
      *error = ErrorReply(408, grpc::StatusCode::DEADLINE_EXCEEDED,
                          "the request did not arrive within " +
                              std::to_string(kIoTimeout.count()) + " s");
      return -1;
    }
    const ssize_t n = recv(fd, buf, size, MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    return n < 0 ? 0 : n;
  }
}

// Reads one request, whole by `deadline`; nullopt (with `error` filled when
// there is something to answer) when none arrived whole.
std::optional<Request> ReadRequest(int fd, Deadline deadline, Reply *error) {
  std::string buf;
  size_t parsed_len = 0;
  const char *method = nullptr, *path = nullptr;
  size_t method_len = 0, path_len = 0, num_headers = 0;
  int minor = 0;
  phr_header headers[kMaxHeaders];
  int header_bytes = -2;
  while (header_bytes == -2) {
    if (buf.size() >= kMaxHeaderBytes) {
      *error = ErrorReply(431, grpc::StatusCode::INVALID_ARGUMENT, "request headers too large");
      return std::nullopt;
    }
    char chunk[4096];
    const ssize_t n = RecvBy(fd, chunk, sizeof(chunk), deadline, error);
    if (n <= 0) {
      return std::nullopt;  // closed: nothing to answer; timed out: 408
    }
    buf.append(chunk, static_cast<size_t>(n));
    num_headers = kMaxHeaders;
    header_bytes = phr_parse_request(buf.data(), buf.size(), &method, &method_len, &path,
                                     &path_len, &minor, headers, &num_headers, parsed_len);
    parsed_len = buf.size();
  }
  if (header_bytes < 0) {
    *error = ErrorReply(400, grpc::StatusCode::INVALID_ARGUMENT, "malformed HTTP request");
    return std::nullopt;
  }
  Request req;
  req.method.assign(method, method_len);
  const std::string_view target(path, path_len);
  const size_t q = target.find('?');
  req.path = target.substr(0, q);
  if (q != std::string_view::npos) {
    req.query = target.substr(q + 1);
  }
  std::optional<size_t> content_length;
  for (size_t i = 0; i < num_headers; i++) {
    const std::string_view name(headers[i].name, headers[i].name_len);
    const std::string_view value(headers[i].value, headers[i].value_len);
    const auto is = [&](const char *want) {
      return name.size() == std::strlen(want) &&
             strncasecmp(name.data(), want, name.size()) == 0;
    };
    if (is("Content-Length")) {
      size_t len = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), len);
      if (ec != std::errc() || end != value.data() + value.size()) {
        *error = ErrorReply(400, grpc::StatusCode::INVALID_ARGUMENT, "bad Content-Length");
        return std::nullopt;
      }
      content_length = len;
    } else if (is("Transfer-Encoding")) {
      *error = ErrorReply(411, grpc::StatusCode::INVALID_ARGUMENT,
                          "send the body with a Content-Length");
      return std::nullopt;
    } else if (is("Last-Event-ID")) {
      req.last_event_id = value;
    } else if (is("Host")) {
      req.host = std::string(value);
    } else if (is("Origin")) {
      req.origin = std::string(value);
    } else if (is("Sec-Fetch-Site")) {
      req.fetch_site = value;
    } else if (is("Content-Type")) {
      req.content_type = value;
    }
  }
  const size_t length = content_length.value_or(0);
  if (length > kMaxBodyBytes) {
    *error = ErrorReply(413, grpc::StatusCode::RESOURCE_EXHAUSTED, "request body too large");
    return std::nullopt;
  }
  req.body = buf.substr(static_cast<size_t>(header_bytes));
  while (req.body.size() < length) {
    char chunk[16384];
    const ssize_t n =
        RecvBy(fd, chunk, std::min(sizeof(chunk), length - req.body.size()), deadline, error);
    if (n <= 0) {
      return std::nullopt;
    }
    req.body.append(chunk, static_cast<size_t>(n));
  }
  req.body.resize(length);
  return req;
}

// Percent-decoding of one query value ('+' is a space).
std::string Decode(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned v = 0;
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size() &&
               std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16).ptr ==
                   s.data() + i + 3) {
      out += static_cast<char>(v);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool ParseU64(std::string_view s, uint64_t *out) {
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
  return ec == std::errc() && end == s.data() + s.size() && !s.empty();
}

const google::protobuf::ServiceDescriptor &ControlService() {
  static const google::protobuf::ServiceDescriptor *service =
      google::protobuf::DescriptorPool::generated_pool()->FindServiceByName(
          v2::Control::service_full_name());
  CHECK(service != nullptr);
  return *service;
}

// -- the routes ---------------------------------------------------------------

// POST /v2/<method>: the JSON body as the request (empty: the empty
// message), sent as gRPC's own clients send it.
Reply CallUnary(const std::shared_ptr<grpc::Channel> &channel,
                const google::protobuf::MethodDescriptor &method, std::string_view body) {
  google::protobuf::MessageFactory *factory =
      google::protobuf::MessageFactory::generated_factory();
  std::unique_ptr<google::protobuf::Message> request(
      factory->GetPrototype(method.input_type())->New());
  std::unique_ptr<google::protobuf::Message> response(
      factory->GetPrototype(method.output_type())->New());
  if (body.find_first_not_of(" \t\r\n") != std::string_view::npos) {
    const absl::Status parsed = google::protobuf::util::JsonStringToMessage(body, request.get());
    if (!parsed.ok()) {
      return ErrorReply(400, grpc::StatusCode::INVALID_ARGUMENT,
                        "body is not a " + std::string(method.input_type()->full_name()) +
                            " in protobuf JSON: " + std::string(parsed.message()));
    }
  }
  const std::string bytes = request->SerializeAsString();
  grpc::Slice slice(bytes);
  grpc::ByteBuffer request_buffer(&slice, 1), response_buffer;
  grpc::ClientContext ctx;
  std::promise<grpc::Status> done;
  grpc::GenericStub stub(channel);
  const std::string path =
      "/" + std::string(ControlService().full_name()) + "/" + std::string(method.name());
  stub.UnaryCall(&ctx, path, grpc::StubOptions(), &request_buffer, &response_buffer,
                 [&done](grpc::Status s) { done.set_value(std::move(s)); });
  const grpc::Status status = done.get_future().get();
  if (!status.ok()) {
    return StatusReply(status, ctx);
  }
  grpc::Slice answer;
  if (!response_buffer.DumpToSingleSlice(&answer).ok() ||
      !response->ParseFromArray(answer.begin(), static_cast<int>(answer.size()))) {
    return ErrorReply(500, grpc::StatusCode::INTERNAL, "unreadable answer");
  }
  Reply reply;
  const absl::Status printed = google::protobuf::util::MessageToJsonString(*response, &reply.body);
  if (!printed.ok()) {
    return ErrorReply(500, grpc::StatusCode::INTERNAL,
                      "answer not printable as JSON: " + std::string(printed.message()));
  }
  reply.body += '\n';
  return reply;
}

// GET /metrics.
Reply Metrics(const std::shared_ptr<grpc::Channel> &channel) {
  grpc::ClientContext ctx;
  v2::ListMetricsResponse response;
  const grpc::Status status =
      v2::Control::NewStub(channel)->ListMetrics(&ctx, v2::ListMetricsRequest(), &response);
  if (!status.ok()) {
    return StatusReply(status, ctx);
  }
  Reply reply;
  reply.content_type = "text/plain; version=0.0.4; charset=utf-8";
  reply.body = MetricsExposition(response);
  return reply;
}

// GET /v2/events: where the stream starts, from the query or else the
// browser's Last-Event-ID ("EPOCH:SEQUENCE", the last event it saw).
std::optional<v2::WatchEventsRequest> ParseWatch(const Request &req, Reply *error) {
  v2::WatchEventsRequest watch;
  std::string_view query = req.query;
  while (!query.empty()) {
    const size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
    const size_t eq = pair.find('=');
    const std::string_view key = pair.substr(0, eq);
    const std::string value =
        eq == std::string_view::npos ? std::string() : Decode(pair.substr(eq + 1));
    uint64_t n = 0;
    if (key == "from_sequence" && ParseU64(value, &n)) {
      watch.set_from_sequence(n);
    } else if (key == "daemon_epoch" && ParseU64(value, &n)) {
      watch.set_daemon_epoch(n);
    } else if (key == "types") {
      std::string_view rest = value;
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        if (comma != 0) {
          watch.add_types(std::string(rest.substr(0, comma)));
        }
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      }
    } else if (!key.empty()) {
      *error = ErrorReply(400, grpc::StatusCode::INVALID_ARGUMENT,
                          "bad query parameter '" + std::string(key) +
                              "' (from_sequence, daemon_epoch: numbers; types: a,b)");
      return std::nullopt;
    }
  }
  if (watch.from_sequence() == 0 && !req.last_event_id.empty()) {
    const std::string_view id = req.last_event_id;
    const size_t colon = id.find(':');
    uint64_t epoch = 0, sequence = 0;
    if (colon != std::string_view::npos && ParseU64(id.substr(0, colon), &epoch) &&
        ParseU64(id.substr(colon + 1), &sequence)) {
      watch.set_daemon_epoch(epoch);
      watch.set_from_sequence(sequence + 1);
    }
  }
  return watch;
}

void StreamEvents(int fd, const std::shared_ptr<grpc::Channel> &channel,
                  const v2::WatchEventsRequest &watch, const std::atomic<bool> &stopping) {
  grpc::ClientContext ctx;
  std::unique_ptr<grpc::ClientReader<v2::Event>> reader =
      v2::Control::NewStub(channel)->WatchEvents(&ctx, watch);
  // The call blocks between events: this watches for the client leaving
  // (or Stop) and cancels it.
  std::atomic<bool> finished{false};
  std::thread watcher([&] {
    while (!finished.load()) {
      pollfd p{fd, POLLIN | POLLRDHUP, 0};
      const int n = poll(&p, 1, kWatchIntervalMs);
      if (stopping.load() || (n > 0 && p.revents != 0)) {
        ctx.TryCancel();
        return;
      }
    }
  });
  bool streaming = SendAll(fd,
                           "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           std::chrono::steady_clock::now() + kIoTimeout);
  v2::Event event;
  std::string message, json;
  while (streaming && reader->Read(&event)) {
    message.clear();
    // The id a reconnect resumes after: an event's sequence; for
    // "bess.start", whose sequence is the next event's, the one before.
    if (event.sequence() > 0) {
      const uint64_t last = event.type() == "bess.start" ? event.sequence() - 1 : event.sequence();
      message = "id: " + std::to_string(event.daemon_epoch()) + ":" + std::to_string(last) + "\n";
    }
    json.clear();
    ignore_result(google::protobuf::util::MessageToJsonString(event, &json));
    message += "data: " + json + "\n\n";
    streaming = SendAll(fd, message, std::chrono::steady_clock::now() + kIoTimeout);
  }
  if (!streaming) {
    ctx.TryCancel();
    while (reader->Read(&event)) {
    }
  }
  ignore_result(reader->Finish());
  finished.store(true);
  watcher.join();
}

// The host name of a Host header value (or an Origin's authority), lower
// case, without port or IPv6 brackets; nullopt when malformed.
std::optional<std::string> HostName(std::string_view host) {
  std::string_view name;
  if (!host.empty() && host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string_view::npos ||
        (close + 1 < host.size() && host[close + 1] != ':')) {
      return std::nullopt;
    }
    name = host.substr(1, close - 1);
  } else {
    name = host.substr(0, host.find(':'));
  }
  if (name.empty()) {
    return std::nullopt;
  }
  std::string out(name);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// "localhost" or an IP address: a name no DNS rebinding produces, since a
// page that rebinds sends its own DNS name, and an address in the URL makes
// the page's origin that address.
bool IsLocalhostOrAddress(const std::string &name) {
  if (name == "localhost") {
    return true;
  }
  in_addr v4{};
  in6_addr v6{};
  return inet_pton(AF_INET, name.c_str(), &v4) == 1 ||
         inet_pton(AF_INET6, name.c_str(), &v6) == 1;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) == std::tolower(y);
  });
}

// What a browser could send on a page's behalf that this daemon refuses
// (D-125), so no web page, DNS rebinding included, can drive it:
// - a Host that is not localhost, an IP address or one of `allowed_hosts`:
//   a page that rebinds its own name to this address still sends its name;
// - an Origin other than the Host, and Sec-Fetch-Site: cross-site;
// - a POST that is not application/json, which no cross-origin form or
//   simple request can send without a preflight this server never allows.
std::optional<Reply> Refuse(const Request &req, const std::vector<std::string> &allowed_hosts) {
  const auto forbid = [](const std::string &why) {
    return ErrorReply(403, grpc::StatusCode::PERMISSION_DENIED, why);
  };
  if (!req.host) {
    return ErrorReply(400, grpc::StatusCode::INVALID_ARGUMENT, "a Host header is required");
  }
  const std::optional<std::string> name = HostName(*req.host);
  if (!name || (!IsLocalhostOrAddress(*name) &&
                std::find(allowed_hosts.begin(), allowed_hosts.end(), *name) ==
                    allowed_hosts.end())) {
    return forbid("Host '" + *req.host +
                  "' is not a name of this daemon (localhost, an IP address, the --http_addr "
                  "host or --http_allowed_hosts)");
  }
  if (req.origin) {
    const size_t scheme = req.origin->find("://");
    const std::string_view authority =
        scheme == std::string::npos ? std::string_view()
                                    : std::string_view(*req.origin).substr(scheme + 3);
    if (scheme == std::string::npos || !EqualsIgnoreCase(authority, *req.host)) {
      return forbid("Origin '" + *req.origin + "' is not this daemon's (" + *req.host + ")");
    }
  }
  if (EqualsIgnoreCase(req.fetch_site, "cross-site")) {
    return forbid("a cross-site request");
  }
  if (req.method == "POST") {
    std::string_view type = req.content_type;
    type = type.substr(0, type.find(';'));
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
      type.remove_suffix(1);
    }
    if (!EqualsIgnoreCase(type, "application/json")) {
      return ErrorReply(415, grpc::StatusCode::INVALID_ARGUMENT,
                        "POST needs Content-Type: application/json");
    }
  }
  return std::nullopt;
}

Reply NotAllowed(const Request &req, const char *allowed) {
  Reply r = ErrorReply(405, grpc::StatusCode::UNIMPLEMENTED,
                       req.method + " " + req.path + ": only " + allowed);
  r.allow = allowed;
  return r;
}

// Every route but the event stream.
Reply Route(const Request &req, const std::shared_ptr<grpc::Channel> &channel) {
  if (req.path == "/" || req.path == "/index.html") {
    if (req.method != "GET") {
      return NotAllowed(req, "GET");
    }
    Reply page;
    page.content_type = "text/html; charset=utf-8";
    page.body = kHttpIndexPage;
    return page;
  }
  if (req.path == "/metrics") {
    return req.method == "GET" ? Metrics(channel) : NotAllowed(req, "GET");
  }
  if (req.path == "/v2/events") {
    return NotAllowed(req, "GET");  // GET is streamed before routing
  }
  if (req.path.starts_with("/v2/")) {
    const std::string name = req.path.substr(4);
    const google::protobuf::MethodDescriptor *method = ControlService().FindMethodByName(name);
    if (method == nullptr || method->client_streaming() || method->server_streaming()) {
      return ErrorReply(404, grpc::StatusCode::NOT_FOUND,
                        "no unary " + std::string(ControlService().full_name()) + " method '" +
                            name + "' (events: GET /v2/events)");
    }
    if (req.method != "POST") {
      return NotAllowed(req, "POST");
    }
    return CallUnary(channel, *method, req.body);
  }
  return ErrorReply(404, grpc::StatusCode::NOT_FOUND, "no such path: " + req.path);
}

}  // namespace

// -- the server ---------------------------------------------------------------

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Bind(const std::string &address, const std::string &allowed_hosts,
                      std::string *error) {
  const size_t colon = address.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == address.size()) {
    *error = "--http_addr '" + address +
             "': expected HOST:PORT, e.g. 127.0.0.1:8080 (0.0.0.0:8080 to serve every interface)";
    return false;
  }
  std::string host = address.substr(0, colon);
  const std::string port = address.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo *found = nullptr;
  if (const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &found); rc != 0) {
    *error = "--http_addr '" + address + "': " + gai_strerror(rc);
    return false;
  }
  std::string last_error = "no address";
  for (const addrinfo *ai = found; ai != nullptr; ai = ai->ai_next) {
    const int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, 128) != 0) {
      last_error = std::strerror(errno);
      close(fd);
      continue;
    }
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len);
    listen_fd_ = fd;
    address_ = FormatAddress(reinterpret_cast<const sockaddr *>(&bound), len);
    port_ = ntohs(bound.ss_family == AF_INET6
                      ? reinterpret_cast<const sockaddr_in6 *>(&bound)->sin6_port
                      : reinterpret_cast<const sockaddr_in *>(&bound)->sin_port);
    if (!IsLoopback(reinterpret_cast<const sockaddr *>(&bound))) {
      LOG(WARNING) << "HTTP on " << address_
                   << ", beyond loopback: whoever reaches it controls this bessd "
                      "(no authentication, no TLS)";
    }
    LOG(INFO) << "HTTP listening on " << address_;
    break;
  }
  freeaddrinfo(found);
  if (listen_fd_ < 0) {
    *error = "--http_addr '" + address + "': " + last_error;
    return false;
  }
  // Host names requests may carry besides localhost and IP addresses: the
  // one --http_addr gave, and the list.
  const auto allow = [this](std::string_view host) {
    const std::optional<std::string> name = HostName(host);
    if (name && !IsLocalhostOrAddress(*name) &&
        std::find(allowed_hosts_.begin(), allowed_hosts_.end(), *name) == allowed_hosts_.end()) {
      allowed_hosts_.push_back(*name);
    }
  };
  allow(address.substr(0, colon));
  std::string_view rest = allowed_hosts;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view entry = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    while (!entry.empty() && entry.front() == ' ') {
      entry.remove_prefix(1);
    }
    while (!entry.empty() && entry.back() == ' ') {
      entry.remove_suffix(1);
    }
    allow(entry);
  }
  return true;
}

void HttpServer::Start(std::shared_ptr<grpc::Channel> channel) {
  CHECK_GE(listen_fd_, 0) << "Bind first";
  channel_ = std::move(channel);
  wake_fd_ = eventfd(0, EFD_CLOEXEC);
  PCHECK(wake_fd_ >= 0) << "eventfd";
  acceptor_ = std::thread([this] { AcceptLoop(); });
}

void HttpServer::Stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  if (acceptor_.joinable()) {
    const uint64_t one = 1;
    ignore_result(write(wake_fd_, &one, sizeof(one)));
    acceptor_.join();
  }
  std::list<std::unique_ptr<Connection>> open;
  {
    std::lock_guard<std::mutex> lock(mu_);
    open.swap(connections_);
  }
  // Ends reads and writes in progress; an event stream's watcher sees the
  // flag and cancels its call.
  for (const auto &c : open) {
    shutdown(c->fd, SHUT_RDWR);
  }
  for (const auto &c : open) {
    c->thread.join();
    close(c->fd);
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (wake_fd_ >= 0) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
}

void HttpServer::ReapLocked() {
  for (auto it = connections_.begin(); it != connections_.end();) {
    if ((*it)->done.load()) {
      (*it)->thread.join();
      close((*it)->fd);
      it = connections_.erase(it);
    } else {
      ++it;
    }
  }
}

void HttpServer::AcceptLoop() {
  while (!stopping_.load()) {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "HTTP poll";
      return;
    }
    if (fds[1].revents != 0) {
      return;
    }
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      continue;  // the client gave up, or out of descriptors for now
    }
    const auto accepted = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mu_);
    ReapLocked();  // joins finished connections, closing their descriptors
    if (connections_.size() >= kMaxConnections) {
      SendReply(fd, ErrorReply(503, grpc::StatusCode::RESOURCE_EXHAUSTED,
                               "too many HTTP connections"));
      close(fd);
      continue;
    }
    auto conn = std::make_unique<Connection>();
    conn->fd = fd;
    conn->accepted = accepted;
    Connection *c = conn.get();
    connections_.push_back(std::move(conn));
    c->thread = std::thread([this, c] { Serve(c); });
  }
}

void HttpServer::Serve(Connection *conn) {
  const int fd = conn->fd;
  Reply error;
  error.status = 0;  // nothing to answer unless a reader sets one
  if (const std::optional<Request> req = ReadRequest(fd, conn->accepted + kIoTimeout, &error)) {
    if (const std::optional<Reply> refused = Refuse(*req, allowed_hosts_)) {
      SendReply(fd, *refused);
    } else if (req->path == "/v2/events" && req->method == "GET") {
      if (const auto watch = ParseWatch(*req, &error)) {
        StreamEvents(fd, channel_, *watch, stopping_);
      } else {
        SendReply(fd, error);
      }
    } else {
      SendReply(fd, Route(*req, channel_));
    }
  } else if (error.status != 0) {
    SendReply(fd, error);
  }
  shutdown(fd, SHUT_WR);  // EOF to the client; the descriptor is closed when reaped
  conn->done.store(true);
}

}  // namespace bess::control
