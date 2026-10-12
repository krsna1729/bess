// SPDX-License-Identifier: BSD-3-Clause

// bessctl: BESS's command line, on the public C++ SDK only (G1.7, D-104).
// Pipelines are manifests: control_v2 Pipeline JSON (manifest.h); the daemon
// validates, diffs, plans and applies them.

#include <pthread.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
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
#include "manifest.h"

namespace {

namespace pb = ::bess::pb::v2;
using bess::sdk::Client;
using bess::sdk::Error;

constexpr std::string_view kUsage = R"(usage: bessctl [-s HOST:PORT] COMMAND [ARGS]

  status                         daemon version, epoch, plugins, ports
  schema [CLASS]                 module classes and port drivers, or one class
  get                            the active pipeline, as a manifest
  validate FILE                  check FILE; print it in canonical form
  diff FILE                      what applying FILE would change
  plan FILE                      the steps applying FILE would run
  apply FILE [--generation G@E]  make FILE the active pipeline (with G@E: only
                                 if the active version is still generation G
                                 of daemon epoch E, as diff/plan/apply print)
  resources                      transactional resources (key -> value types)
  metrics                        metric samples
  events [--from SEQ] [TYPE...]  follow operational events until interrupted

A manifest is a control_v2 Pipeline in protobuf JSON; a module's or port's
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

  int Get() {
    auto schema = LoadSchema();
    if (!schema) {
      return 1;
    }
    auto snap = client_->Pipeline();
    if (!snap) {
      return Fail(snap.error());
    }
    auto text = (*schema)->PipelineToJson(snap->pipeline);
    if (!text) {
      return Fail(text.error());
    }
    std::println("{}", *text);
    return 0;
  }

  int Validate(const std::string &path) {
    auto schema = LoadSchema();
    auto m = schema ? ReadManifest(**schema, path) : std::nullopt;
    if (!m) {
      return 1;
    }
    auto normalized = client_->ValidatePipeline(m->pipeline);
    if (!normalized) {
      return Fail(normalized.error());
    }
    auto text = (*schema)->PipelineToJson(*normalized);
    if (!text) {
      return Fail(text.error());
    }
    std::println("{}", *text);
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
    auto m = schema.ManifestFromJson(*text);
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

int main(int argc, char **argv) {
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
  if (command == "get" && rest.empty()) {
    return cli.Get();
  }
  if (command == "validate") {
    return one_file([&](const std::string &f) { return cli.Validate(f); });
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
  return Usage();
}
