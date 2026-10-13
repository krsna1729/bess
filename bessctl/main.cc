// SPDX-License-Identifier: BSD-3-Clause

// bessctl: BESS's command line, on the public C++ SDK only (G1.7, D-104).
// Pipelines are manifests: control_v2 Pipeline JSON or YAML (manifest.h); the daemon
// validates, diffs, plans and applies them.

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "bess/sdk/client.h"
#include "cli.h"
#include "manifest.h"

namespace {

namespace pb = ::bess::pb::v2;
using bess::sdk::Client;
using bess::sdk::Error;

constexpr std::string_view kUsage = R"(usage: bessctl [-s HOST:PORT] COMMAND [ARGS]

  status                         daemon version, epoch, plugins, ports
  schema [CLASS]                 module classes and port drivers, or one class
  get [-o json|yaml]             the active pipeline, as a manifest
  validate FILE [-o json|yaml]   check FILE; print it in canonical form
  diff FILE                      what applying FILE would change
  plan FILE                      the steps applying FILE would run
  apply FILE [--generation G@E]  make FILE the active pipeline (with G@E: only
                                 if the active version is still generation G
                                 of daemon epoch E, as diff/plan/apply print)
  resources                      transactional resources (key -> value types)
  metrics                        metric samples
  events [--from SEQ] [TYPE...]  follow operational events until interrupted
  ports [NAME...]                link state and counters of the ports
  pools                          the default packet pool (on the memory node)
  plugin import|unload PATH      load or unload a plugin (a path on the
                                 daemon's host)
  capture MODULE in|out[:GATE] [--pcapng]
                                 copy the packets on MODULE's input or output
                                 gates (all, or GATE) to standard output as
                                 pcap (or pcapng) until interrupted, e.g.
                                 | tcpdump -r -  (pcap of a module with more
                                 than one gate there needs a GATE;
                                 the daemon must share this host's /tmp)
  command MODULE CMD [ARG] [-o json|yaml]
                                 run MODULE's command CMD (schema CLASS lists
                                 them) with ARG, JSON or YAML text of the
                                 command's argument type (default {}); print
                                 its answer. A command not marked thread-safe
                                 needs the workers paused
  pause | resume                 pause or resume every worker (a worker a
                                 pipeline creates starts paused)
  shutdown                       stop the daemon

A manifest is a control_v2 Pipeline in protobuf JSON or YAML (by extension,
.json or .yaml/.yml, else JSON if it starts with "{"); a module's or port's
"arg" may leave out "@type". FILE "-" is standard input. The daemon is
-s, else $BESS_SERVER, else localhost:10514.
)";

int Usage() {
  std::print(stderr, "{}", kUsage);
  return 2;
}

int Fail(std::string_view what) {
  std::fflush(stdout);  // what was printed before the failure, first
  std::println(stderr, "bessctl: {}", what);
  return 1;
}

// A pipeline version as `apply --generation` reads it back: generation@epoch.
// Generations restart with the daemon, so one alone is ambiguous (D-106).
std::string Version(const bess::sdk::PipelineVersion &v) {
  return std::format("{}@{}", v.generation, v.daemon_epoch);
}

std::string Describe(const Error &e) {
  std::string text = e.message;
  if (e.detail) {
    if (!e.detail->object().empty()) {
      text += " (object " + e.detail->object() + ")";
    }
    if (!e.detail->field().empty()) {
      text += " (field " + e.detail->field() + ")";
    }
  }
  for (const bess::sdk::OpFailure &f : e.failures) {
    text += std::format("; operation {}: {}", f.index, f.error);
  }
  return text;
}

int Fail(const Error &e) {
  return Fail(Describe(e));
}

std::optional<std::string> ReadInput(const std::string &path) {
  if (path == "-") {
    return std::string(std::istreambuf_iterator<char>(std::cin), {});
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in), {});
}

class Cli {
 public:
  explicit Cli(std::unique_ptr<Client> client) : client_(std::move(client)) {}

  int Status() {
    auto caps = client_->Capabilities();
    if (!caps) {
      return Fail(caps.error());
    }
    std::println("daemon       {}", caps->daemon_version());
    std::println("epoch        {}", caps->daemon_epoch());
    std::println("plugin API   {}", caps->plugin_api_version());
    if (!caps->placement().empty()) {  // a daemon before D-123 sends none
      std::println("memory node  {}", caps->memory_node());
      std::println("{}", caps->placement());
    }
    if (!caps->simd().empty()) {  // a daemon before D-126 sends none
      std::println("{}", caps->simd());
    }
    std::println("classes      {}", caps->module_classes_size());
    for (const pb::PluginInfo &p : caps->plugins()) {
      std::println("plugin       {} {} ({})", p.name(), p.version(), p.path());
    }
    for (const pb::PortInfo &p : caps->ports()) {
      std::println("port         {} driver {} rx {} tx {}", p.name(), p.driver(), p.rx_queues(),
                   p.tx_queues());
    }
    return 0;
  }

  int Ports(std::span<const std::string> names) {
    auto ports = client_->Ports(std::vector<std::string>(names.begin(), names.end()));
    if (!ports) {
      return Fail(ports.error());
    }
    for (const pb::PortStatus &p : *ports) {
      const pb::PortStatus::Link &l = p.link();
      std::println("{} ({}): link {}{}{}", p.name(), p.driver(), l.up() ? "up" : "down",
                   l.speed_mbps() ? std::format(" {} Mb/s{}{}", l.speed_mbps(),
                                                l.full_duplex() ? " full-duplex" : " half-duplex",
                                                l.autoneg() ? " autoneg" : "")
                                  : std::string(),
                   p.numa_node() < 0 ? std::string()
                                     : std::format(", NUMA node {}{}", p.numa_node(),
                                                   p.cross_numa() ? " (cross-NUMA)" : ""));
      std::println("  rx  packets {} bytes {} dropped {}", p.rx().packets(), p.rx().bytes(),
                   p.rx().dropped());
      std::println("  tx  packets {} bytes {} dropped {} prepare errors {}", p.tx().packets(),
                   p.tx().bytes(), p.tx().dropped(), p.tx().prepare_errors());
    }
    return 0;
  }

  int Pools() {
    auto pools = client_->PacketPools();
    if (!pools) {
      return Fail(pools.error());
    }
    for (const pb::PacketPool &p : *pools) {
      std::println("socket {}: {} packets of {} bytes ({} populated), {} in use, {} available, "
                   "cache {}",
                   p.socket(), p.size(), p.element_size(), p.populated(), p.in_use(),
                   p.available(), p.cache_size());
    }
    return 0;
  }

  int Plugin(const std::string &op, const std::string &path) {
    auto done = op == "import" ? client_->ImportPlugin(path) : client_->UnloadPlugin(path);
    if (!done) {
      return Fail(done.error());
    }
    return 0;
  }

  int Workers(bool resume) {
    if (auto done = resume ? client_->ResumeWorkers() : client_->PauseWorkers(); !done) {
      return Fail(done.error());
    }
    return 0;
  }

  int Shutdown() {
    if (auto done = client_->Shutdown(); !done) {
      return Fail(done.error());
    }
    return 0;
  }

  // Captures into a FIFO this process makes and copies it to standard output
  // until `stop`, the end of the stream, or a reader of standard output that
  // went away; then stops the capture (always, once it started).
  int Capture(pb::SetCaptureRequest req, std::stop_token stop) {
    char dir[] = "/tmp/bessctl-capture-XXXXXX";
    if (mkdtemp(dir) == nullptr) {
      return Fail(std::format("mkdtemp: {}", std::strerror(errno)));
    }
    const std::string fifo = std::string(dir) + "/capture";
    struct Cleanup {
      std::string dir, fifo;
      int fd = -1;
      ~Cleanup() {
        if (fd >= 0) {
          close(fd);
        }
        unlink(fifo.c_str());
        rmdir(dir.c_str());
      }
    } cleanup{dir, fifo};
    if (mkfifo(fifo.c_str(), 0600) != 0) {
      return Fail(std::format("mkfifo {}: {}", fifo, std::strerror(errno)));
    }
    // The read end first: the daemon opens the write end without blocking,
    // which fails while nobody reads.
    cleanup.fd = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
    if (cleanup.fd < 0) {
      return Fail(std::format("open {}: {}", fifo, std::strerror(errno)));
    }
    req.set_fifo(fifo);
    req.set_enable(true);
    if (auto started = client_->SetCapture(req); !started) {
      return Fail(started.error());
    }
    std::signal(SIGPIPE, SIG_IGN);  // a reader that goes away is EPIPE, not death
    int rc = 0;
    std::vector<char> buf(1 << 16);
    while (!stop.stop_requested()) {
      pollfd p{.fd = cleanup.fd, .events = POLLIN, .revents = 0};
      if (poll(&p, 1, 200) <= 0) {
        continue;  // timeout, or a signal: look at `stop` again
      }
      const ssize_t n = read(cleanup.fd, buf.data(), buf.size());
      if (n == 0) {
        break;  // the daemon closed it (the capture was stopped elsewhere)
      }
      if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
          continue;
        }
        rc = Fail(std::format("read {}: {}", fifo, std::strerror(errno)));
        break;
      }
      if (!WriteAll(STDOUT_FILENO, buf.data(), static_cast<size_t>(n))) {
        break;  // whoever read standard output is gone
      }
    }
    req.clear_fifo();
    req.set_enable(false);
    if (auto stopped = client_->SetCapture(req); !stopped) {
      return Fail(std::format("the capture is still running: {}", Describe(stopped.error())));
    }
    return rc;
  }

 private:
  static bool WriteAll(int fd, const char *data, size_t size) {
    while (size > 0) {
      const ssize_t n = write(fd, data, size);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return false;
      }
      data += n;
      size -= static_cast<size_t>(n);
    }
    return true;
  }

 public:

  int ShowSchema(std::span<const std::string> args) {
    auto schema = client_->Schema();
    if (!schema) {
      return Fail(schema.error());
    }
    if (args.empty()) {
      for (const pb::ModuleClassSchema &c : schema->module_classes()) {
        std::println("module  {:<24} {}", c.name(), c.init_arg_type());
      }
      for (const pb::PortDriverSchema &d : schema->port_drivers()) {
        std::println("driver  {:<24} {}", d.name(), d.init_arg_type());
      }
      return 0;
    }
    for (const pb::ModuleClassSchema &c : schema->module_classes()) {
      if (c.name() != args[0]) {
        continue;
      }
      std::println("{}: {}", c.name(), c.help());
      std::println("  init      {}", c.init_arg_type());
      std::println("  gates     in {} out {}", c.igates(), c.ogates());
      for (const pb::CommandSchema &cmd : c.commands()) {
        std::println("  command   {:<20} {}{}", cmd.name(), cmd.arg_type(),
                     cmd.thread_safe() ? "  (no pause)" : "");
      }
      return 0;
    }
    for (const pb::PortDriverSchema &d : schema->port_drivers()) {
      if (d.name() == args[0]) {
        std::println("{}: {}", d.name(), d.help());
        std::println("  init      {}", d.init_arg_type());
        return 0;
      }
    }
    return Fail("no module class or port driver \"" + args[0] + "\"");
  }

  // A pipeline as a manifest, on standard output.
  static int PrintPipeline(const bessctl::Schema &schema, const pb::Pipeline &p,
                           bessctl::Format format) {
    auto text = schema.PipelineTo(p, format);
    if (!text) {
      return Fail(text.error());
    }
    std::print("{}{}", *text, format == bessctl::Format::kJson ? "\n" : "");
    return 0;
  }

  int Get(bessctl::Format format) {
    auto schema = LoadSchema();
    if (!schema) {
      return 1;
    }
    auto snap = client_->Pipeline();
    if (!snap) {
      return Fail(snap.error());
    }
    return PrintPipeline(**schema, snap->pipeline, format);
  }

  int Validate(const std::string &path, bessctl::Format format) {
    auto schema = LoadSchema();
    auto m = schema ? ReadManifest(**schema, path) : std::nullopt;
    if (!m) {
      return 1;
    }
    auto normalized = client_->ValidatePipeline(m->pipeline);
    if (!normalized) {
      return Fail(normalized.error());
    }
    if (const int rc = PrintPipeline(**schema, *normalized, format); rc != 0) {
      return rc;
    }
    NoteResources(*m);
    return 0;
  }

  int Diff(const std::string &path) {
    auto schema = LoadSchema();
    auto m = schema ? ReadManifest(**schema, path) : std::nullopt;
    if (!m) {
      return 1;
    }
    auto diff = client_->DiffPipeline(m->pipeline);
    if (!diff) {
      return Fail(diff.error());
    }
    for (const pb::Change &c : diff->diff.changes()) {
      std::println("{:<14} {:<14} {}", pb::ChangeKind_Name(c.kind()),
                   pb::ObjectType_Name(c.type()), c.object());
    }
    std::println("# against version {}: {} change(s)", Version(diff->version),
                 diff->diff.changes_size());
    NoteResources(*m);
    return 0;
  }

  int Plan(const std::string &path) {
    auto schema = LoadSchema();
    auto m = schema ? ReadManifest(**schema, path) : std::nullopt;
    if (!m) {
      return 1;
    }
    auto plan = client_->PlanPipeline(m->pipeline);
    if (!plan) {
      return Fail(plan.error());
    }
    for (const pb::PlanStep &s : plan->steps) {
      std::println("{:<8} {:<20} {:<14} {}", pb::PlanPhase_Name(s.phase()), s.operation(),
                   pb::ObjectType_Name(s.type()), s.object());
    }
    std::println("# against version {}: {} step(s)", Version(plan->version),
                 plan->steps.size());
    NoteResources(*m);
    return 0;
  }

  // The pipeline, then the manifest's resource entries in one transaction:
  // its resources exist only once the pipeline does. Both steps are
  // idempotent, so after a failed second step the same apply is the retry
  // (with --generation, at the version the first step reached).
  int Apply(const std::string &path, std::optional<bess::sdk::PipelineVersion> expected) {
    auto schema = LoadSchema();
    auto m = schema ? ReadManifest(**schema, path) : std::nullopt;
    if (!m) {
      return 1;
    }
    auto r = client_->ApplyPipeline(m->pipeline, expected);
    if (!r) {
      return Fail(r.error());
    }
    const bess::sdk::PipelineVersion reached{r->generation(), r->daemon_epoch()};
    std::println("version {}: {} operation(s){}; validate {} us, prepare {} us, commit {} us, "
                 "retire {} us",
                 Version(reached), r->applied_ops(), r->workers_paused() ? ", workers paused" : "",
                 r->validation_us(), r->prepare_us(), r->paused_commit_us(), r->retire_us());
    if (m->resources.empty()) {
      return 0;
    }
    auto committed = CommitEntries(**schema, *m);
    if (!committed) {
      return Fail(std::format(
          "the pipeline is applied, its resource entries are not: {} (fix and apply again{})",
          committed.error(),
          expected ? std::format(" with --generation {}", Version(reached)) : ""));
    }
    std::println("resources: {} entr{} in {} resource(s); transaction {}, resource generation {}",
                 m->entries(), m->entries() == 1 ? "y" : "ies", m->resources.size(),
                 committed->request_id, committed->generation);
    return 0;
  }

  int Command(const std::string &module, const std::string &command, const std::string &arg,
              bessctl::Format format) {
    auto schema = LoadSchema();
    if (!schema) {
      return 1;
    }
    auto snapshot = client_->Pipeline();
    if (!snapshot) {
      return Fail(snapshot.error());
    }
    const auto &modules = snapshot->pipeline.modules();
    const auto m = std::find_if(modules.begin(), modules.end(),
                                [&](const pb::Module &x) { return x.name() == module; });
    if (m == modules.end()) {
      return Fail(std::format("no module \"{}\"", module));
    }
    auto type = (*schema)->CommandArgType(m->mclass(), command);
    if (!type) {
      return Fail(type.error());
    }
    auto message = (*schema)->MessageFromText(*type, arg);
    if (!message) {
      return Fail(std::format("{} {}: argument ({}): {}", module, command, *type, message.error()));
    }
    auto answer = client_->RunModuleCommand(module, command, **message);
    if (!answer) {
      return Fail(answer.error());
    }
    if (answer->type_url().empty()) {
      return 0;  // the command answers nothing
    }
    auto text = (*schema)->AnyTo(*answer, format);
    if (!text) {
      return Fail(text.error());
    }
    std::print("{}{}", *text, text->ends_with('\n') ? "" : "\n");
    return 0;
  }

  int Resources() {
    auto resources = client_->Resources(true);
    if (!resources) {
      return Fail(resources.error());
    }
    for (const auto &[name, r] : *resources) {
      std::println("{:<32} {} -> {}", name, r.key_type, r.value_type);
    }
    return 0;
  }

  int Metrics() {
    auto samples = client_->Metrics();
    if (!samples) {
      return Fail(samples.error());
    }
    for (const pb::MetricSample &s : *samples) {
      std::string labels;
      for (const auto &[k, v] : s.labels()) {
        labels += (labels.empty() ? "{" : ",") + k + "=\"" + v + "\"";
      }
      if (!labels.empty()) {
        labels += "}";
      }
      std::println("{}{} {}", s.name(), labels, s.value());
    }
    return 0;
  }

  int Events(uint64_t from, const std::vector<std::string> &types, std::stop_token stop) {
    auto r = client_->WatchEvents(
        from, 0, types,
        [](const pb::Event &e) {
          std::string fields;
          for (const auto &[k, v] : e.fields()) {
            fields += " " + k + "=" + v;
          }
          std::println("{} {} {}{}", e.sequence(), e.type(), e.source(), fields);
          std::fflush(stdout);
          return true;
        },
        stop);
    return r ? 0 : Fail(r.error());
  }

 private:
  std::optional<std::unique_ptr<bessctl::Schema>> LoadSchema() {
    auto response = client_->Schema();
    if (!response) {
      Fail(response.error());
      return std::nullopt;
    }
    auto schema = bessctl::Schema::From(*response);
    if (!schema) {
      Fail(schema.error());
      return std::nullopt;
    }
    return std::move(*schema);
  }

  static std::optional<bessctl::Manifest> ReadManifest(const bessctl::Schema &schema,
                                                       const std::string &path) {
    auto text = ReadInput(path);
    if (!text) {
      Fail("cannot read " + path);
      return std::nullopt;
    }
    auto m = schema.ManifestFrom(*text, bessctl::FormatOf(path, *text));
    if (!m) {
      Fail(path + ": " + m.error());
      return std::nullopt;
    }
    return std::move(*m);
  }

  // Validate, diff and plan cover the pipeline only: a resource's types are
  // known once the module that creates it exists. (To stderr: stdout may be
  // a manifest.)
  static void NoteResources(const bessctl::Manifest &m) {
    if (!m.resources.empty()) {
      std::println(stderr,
                   "bessctl: resources: {} entr{} not checked (apply commits them after the "
                   "pipeline)",
                   m.entries(), m.entries() == 1 ? "y" : "ies");
    }
  }

  std::expected<bess::sdk::Applied, std::string> CommitEntries(const bessctl::Schema &schema,
                                                               const bessctl::Manifest &m) {
    auto resources = client_->Resources(true);
    if (!resources) {
      return std::unexpected(Describe(resources.error()));
    }
    bess::sdk::Transaction txn = client_->NewTransaction();
    std::vector<std::string> ops;  // operation index -> the entry it came from
    for (const auto &[name, entries] : m.resources) {
      const auto r = resources->find(name);
      if (r == resources->end()) {
        return std::unexpected(std::format("no resource \"{}\" (bessctl resources lists them)",
                                           name));
      }
      for (size_t i = 0; i < entries.size(); i++) {
        const std::string where = std::format("resource \"{}\" entry {}", name, i);
        auto key = schema.MessageFromJson(r->second.key_type, entries[i].key);
        if (!key) {
          return std::unexpected(std::format("{}: key: {}", where, key.error()));
        }
        auto value = schema.MessageFromJson(r->second.value_type, entries[i].value);
        if (!value) {
          return std::unexpected(std::format("{}: value: {}", where, value.error()));
        }
        if (auto added = txn.Upsert(r->second, **key, **value); !added) {
          return std::unexpected(std::format("{}: {}", where, Describe(added.error())));
        }
        ops.push_back(where);
      }
    }
    auto applied = txn.Commit();
    if (!applied) {
      // The SDK's message lists failures by operation; name the entries.
      const auto &failures = applied.error().failures;
      if (failures.empty()) {
        return std::unexpected(Describe(applied.error()));
      }
      std::string text = "rejected";
      for (const bess::sdk::OpFailure &f : failures) {
        const bool known = f.index >= 0 && static_cast<size_t>(f.index) < ops.size();
        text += std::format("; {}: {}",
                            known ? ops[f.index] : std::format("operation {}", f.index), f.error);
      }
      return std::unexpected(text);
    }
    return std::move(*applied);
  }

  std::unique_ptr<Client> client_;
};

// Ctrl-C ends `events` cleanly (its stream is cancelled, outside any signal
// handler) and any other command at once.
std::stop_source interrupted;
std::atomic<bool> watching{false};

// Before gRPC starts threads, so they inherit the mask and only this thread
// takes the signals.
void HandleSignalsOnAThread() {
  static sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  std::thread([] {
    int sig = 0;
    sigwait(&set, &sig);
    if (!watching.load()) {
      std::_Exit(128 + sig);
    }
    interrupted.request_stop();
  }).detach();
}

std::optional<uint64_t> ParseU64(std::string_view s) {
  uint64_t v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || end != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

// "G@E", as Version prints it: a generation means nothing without its epoch
// (D-106).
std::optional<bess::sdk::PipelineVersion> ParseVersion(std::string_view s) {
  const size_t at = s.find('@');
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  auto generation = ParseU64(s.substr(0, at));
  auto epoch = ParseU64(s.substr(at + 1));
  if (!generation || !epoch || *epoch == 0) {
    return std::nullopt;
  }
  return bess::sdk::PipelineVersion{*generation, *epoch};
}

}  // namespace

int bess::ctl::Main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string server = std::getenv("BESS_SERVER") ? std::getenv("BESS_SERVER") : "localhost:10514";
  if (args.size() >= 2 && (args[0] == "-s" || args[0] == "--server")) {
    server = args[1];
    args.erase(args.begin(), args.begin() + 2);
  }
  if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    return args.empty() ? Usage() : (std::print("{}", kUsage), 0);
  }
  const std::string command = args[0];
  const std::span<const std::string> rest(args.begin() + 1, args.end());

  HandleSignalsOnAThread();
  Cli cli(Client::Dial(server));
  const auto one_file = [&](auto fn) {
    return rest.size() == 1 ? fn(rest[0]) : Usage();
  };
  if (command == "status" && rest.empty()) {
    return cli.Status();
  }
  if (command == "schema" && rest.size() <= 1) {
    return cli.ShowSchema(rest);
  }
  // get, validate and command print JSON, or YAML with -o yaml.
  std::vector<std::string> printed(rest.begin(), rest.end());
  auto output = bessctl::Format::kJson;
  if ((command == "get" || command == "validate" || command == "command") && printed.size() >= 2 &&
      printed[printed.size() - 2] == "-o") {
    const std::string &name = printed.back();
    if (name != "json" && name != "yaml") {
      return Usage();
    }
    output = name == "yaml" ? bessctl::Format::kYaml : bessctl::Format::kJson;
    printed.resize(printed.size() - 2);
  }
  if (command == "get" && printed.empty()) {
    return cli.Get(output);
  }
  if (command == "validate" && printed.size() == 1) {
    return cli.Validate(printed[0], output);
  }
  if (command == "command" && (printed.size() == 2 || printed.size() == 3)) {
    return cli.Command(printed[0], printed[1], printed.size() == 3 ? printed[2] : "{}", output);
  }
  if (command == "diff") {
    return one_file([&](const std::string &f) { return cli.Diff(f); });
  }
  if (command == "plan") {
    return one_file([&](const std::string &f) { return cli.Plan(f); });
  }
  if (command == "apply" && (rest.size() == 1 || (rest.size() == 3 && rest[1] == "--generation"))) {
    std::optional<bess::sdk::PipelineVersion> expected;
    if (rest.size() == 3) {
      expected = ParseVersion(rest[2]);
      if (!expected) {
        std::println(stderr, "--generation takes GENERATION@EPOCH, as diff, plan and apply print it");
        return Usage();
      }
    }
    return cli.Apply(rest[0], expected);
  }
  if (command == "resources" && rest.empty()) {
    return cli.Resources();
  }
  if (command == "metrics" && rest.empty()) {
    return cli.Metrics();
  }
  if (command == "events") {
    uint64_t from = 0;
    std::vector<std::string> types;
    for (size_t i = 0; i < rest.size(); i++) {
      if (rest[i] == "--from") {
        auto v = i + 1 < rest.size() ? ParseU64(rest[++i]) : std::nullopt;
        if (!v) {
          return Usage();
        }
        from = *v;
      } else {
        types.push_back(rest[i]);
      }
    }
    watching = true;
    return cli.Events(from, types, interrupted.get_token());
  }
  if (command == "ports") {
    return cli.Ports(rest);
  }
  if (command == "pools" && rest.empty()) {
    return cli.Pools();
  }
  if (command == "plugin" && rest.size() == 2 && (rest[0] == "import" || rest[0] == "unload")) {
    return cli.Plugin(rest[0], rest[1]);
  }
  if ((command == "pause" || command == "resume") && rest.empty()) {
    return cli.Workers(command == "resume");
  }
  if (command == "shutdown" && rest.empty()) {
    return cli.Shutdown();
  }
  if (command == "capture" && (rest.size() == 2 || (rest.size() == 3 && rest[2] == "--pcapng"))) {
    bess::pb::v2::SetCaptureRequest req;
    req.set_module(rest[0]);
    const std::string_view where = rest[1];
    const std::string_view direction = where.substr(0, where.find(':'));
    if (direction == "in") {
      req.set_direction(bess::pb::v2::SetCaptureRequest::DIRECTION_INPUT);
    } else if (direction == "out") {
      req.set_direction(bess::pb::v2::SetCaptureRequest::DIRECTION_OUTPUT);
    } else {
      return Usage();
    }
    if (direction.size() < where.size()) {
      const auto gate = ParseU64(where.substr(direction.size() + 1));
      if (!gate || *gate > UINT32_MAX) {
        return Usage();
      }
      req.set_gate(static_cast<uint32_t>(*gate));
    }
    req.set_format(rest.size() == 3 ? bess::pb::v2::SetCaptureRequest::FORMAT_PCAPNG
                                    : bess::pb::v2::SetCaptureRequest::FORMAT_PCAP);
    watching = true;  // Ctrl-C stops the capture before exiting
    return cli.Capture(std::move(req), interrupted.get_token());
  }
  return Usage();
}
