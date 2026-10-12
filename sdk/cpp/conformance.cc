// SPDX-License-Identifier: BSD-3-Clause

// The C++ SDK's driver of the control SDK conformance suite (D-104): runs
// sdk/conformance/cases.json against a live bessd, the steps as defined in
// docs/control-sdk.md ("Conformance suite"). sdk/go/bess/conformance_test.go is
// the Go one; bessctl/module_tests/control_sdk_conformance.py runs both.
//
//   sdk_cpp_conformance <address> <resource> <cases.json>
//
// Prints "PASS <case>" or "FAIL <case>: <why>" per case. Exit 0: all passed.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bess/sdk/client.h"

namespace {

namespace v2 = ::bess::pb::v2;
namespace gp = google::protobuf;
using bess::sdk::Client;
using bess::sdk::Error;
using bess::sdk::ErrorKind;

// A step's failure, or nullopt.
using Outcome = std::optional<std::string>;

std::string KindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kInvalidRequest:
      return "invalid_request";
    case ErrorKind::kConflict:
      return "conflict";
    case ErrorKind::kRejected:
      return "rejected";
    case ErrorKind::kBusy:
      return "busy";
    case ErrorKind::kTransport:
      return "transport";
    case ErrorKind::kPipelineConflict:
      return "pipeline_conflict";
    case ErrorKind::kDaemonRestarted:
      return "daemon_restarted";
    case ErrorKind::kStaleResource:
      return "stale_resource";
    case ErrorKind::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

// Whether a call that returned `error` (nullptr: success) is what the step
// wants: `want` an error kind, or "" for success.
Outcome Expect(const Error *error, const std::string &want, std::string_view what) {
  if (error == nullptr) {
    return want.empty() ? Outcome() : std::format("{}: succeeded, want {}", what, want);
  }
  if (want.empty() || KindName(error->kind) != want) {
    return std::format("{}: {} ({}){}", what, KindName(error->kind), error->message,
                       want.empty() ? "" : ", want " + want);
  }
  return std::nullopt;
}

// -- the cases file, read as google.protobuf.Struct --

const gp::Value *Field(const gp::Struct &s, const std::string &name) {
  auto it = s.fields().find(name);
  return it == s.fields().end() ? nullptr : &it->second;
}

std::string Str(const gp::Struct &s, const std::string &name) {
  const gp::Value *v = Field(s, name);
  return v == nullptr ? "" : v->string_value();
}

bool Bool(const gp::Struct &s, const std::string &name) {
  const gp::Value *v = Field(s, name);
  return v != nullptr && v->bool_value();
}

std::vector<const gp::Value *> List(const gp::Struct &s, const std::string &name) {
  std::vector<const gp::Value *> out;
  if (const gp::Value *v = Field(s, name)) {
    for (const gp::Value &e : v->list_value().values()) {
      out.push_back(&e);
    }
  }
  return out;
}

struct Watch {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<v2::Event> events;
  std::jthread thread;  // last: stopped and joined before the rest goes
};

struct Commit {
  std::string request_id;
  uint64_t generation = 0;
};

struct Pipe {
  v2::Pipeline pipeline;
  bess::sdk::PipelineVersion version;
};

class Runner {
 public:
  Runner(Client &client, std::string resource) : client_(client), resource_(std::move(resource)) {}

  Outcome LoadSchema() {
    auto schema = client_.Schema();
    if (!schema) {
      return "schema: " + schema.error().message;
    }
    schema_ = *schema;
    for (const gp::FileDescriptorProto &file : schema_.descriptors().file()) {
      if (pool_.BuildFile(file) == nullptr) {
        return "schema: cannot build " + file.name();
      }
    }
    return std::nullopt;
  }

  Outcome RunCase(const gp::Struct &c) {
    commits_.clear();
    pipes_.clear();
    watches_.clear();
    int index = 0;
    for (const gp::Value *step : List(c, "steps")) {
      if (Outcome failed = Step(step->struct_value())) {
        return std::format("step {} ({}): {}", index, Str(step->struct_value(), "do"), *failed);
      }
      index++;
    }
    watches_.clear();
    return std::nullopt;
  }

 private:
  std::string Var(const std::string &s) const { return s == "$resource" ? resource_ : s; }

  std::unique_ptr<gp::Message> Make(const std::string &type, const gp::Value &json,
                                    std::string *error) {
    const gp::Descriptor *d = pool_.FindMessageTypeByName(type);
    if (d == nullptr) {
      *error = "no type " + type + " in the schema";
      return nullptr;
    }
    std::string text;
    (void)gp::util::MessageToJsonString(json.struct_value(), &text);
    std::unique_ptr<gp::Message> m(factory_.GetPrototype(d)->New());
    if (auto status = gp::util::JsonStringToMessage(text, m.get()); !status.ok()) {
      // A string_view or a StringPiece, by protobuf version: copy it.
      *error = std::format("{} from {}: {}", type, text, std::string(status.message()));
      return nullptr;
    }
    return m;
  }

  Outcome Step(const gp::Struct &s) {
    const std::string what = Str(s, "do");
    const std::string want_error = Str(s, "want_error");
    if (what == "capabilities") {
      auto caps = client_.Capabilities();
      if (!caps) {
        return Expect(&caps.error(), want_error, what);
      }
      if (caps->daemon_version().empty() || caps->module_classes_size() == 0) {
        return "no daemon version or module classes";
      }
      return std::nullopt;
    }
    if (what == "supports") {
      auto supported = client_.Supports(Str(s, "rpc"));
      if (!supported) {
        return Expect(&supported.error(), want_error, what);
      }
      if (*supported != Bool(s, "want")) {
        return std::format("Supports({}) = {}", Str(s, "rpc"), *supported);
      }
      return std::nullopt;
    }
    if (what == "metrics") {
      auto samples = client_.Metrics();
      if (!samples) {
        return Expect(&samples.error(), want_error, what);
      }
      return samples->empty() ? Outcome("no samples") : std::nullopt;
    }
    if (what == "schema") {
      for (const v2::ModuleClassSchema &c : schema_.module_classes()) {
        if (c.name() == Str(s, "class")) {
          return pool_.FindMessageTypeByName(c.init_arg_type()) != nullptr
                     ? std::nullopt
                     : Outcome("init type '" + c.init_arg_type() + "' not in the descriptors");
        }
      }
      return "no class " + Str(s, "class");
    }
    if (what == "resource") {
      auto r = client_.Resource(Var(Str(s, "name")));
      return Expect(r ? nullptr : &r.error(), want_error, what);
    }
    if (what == "watch") {
      return OpenWatch(s);
    }
    if (what == "event") {
      return AwaitEvent(s);
    }
    if (what == "commit") {
      return DoCommit(s, want_error);
    }
    if (what == "transaction_status") {
      const std::string id =
          Field(s, "of") != nullptr ? commits_[Str(s, "of")].request_id : Str(s, "request_id");
      auto status = client_.GetTransaction(id);
      if (!status) {
        return Expect(&status.error(), want_error, what);
      }
      if (status->known != Bool(s, "want_known")) {
        return std::format("known = {}", status->known);
      }
      const std::string outcome = v2::TransactionRecord_Outcome_Name(status->record.outcome());
      if (Field(s, "want_outcome") != nullptr && outcome != Str(s, "want_outcome")) {
        return "outcome " + outcome;
      }
      return std::nullopt;
    }
    if (what == "pipeline") {
      return DoPipeline(s, want_error);
    }
    return "unknown step '" + what + "'";
  }

  Outcome OpenWatch(const gp::Struct &s) {
    std::vector<std::string> types;
    for (const gp::Value *t : List(s, "types")) {
      types.push_back(t->string_value());
    }
    auto w = std::make_unique<Watch>();
    Watch *raw = w.get();
    raw->thread = std::jthread([this, raw, types](std::stop_token stop) {
      // From sequence 1: what the daemon holds, then live; events are matched
      // by request id, so no race with what the case does next.
      (void)client_.WatchEvents(
          1, 0, types,
          [raw](const v2::Event &e) {
            std::lock_guard lock(raw->mu);
            raw->events.push_back(e);
            raw->cv.notify_all();
            return true;
          },
          stop);
    });
    watches_[Str(s, "as")] = std::move(w);
    return std::nullopt;
  }

  Outcome AwaitEvent(const gp::Struct &s) {
    auto w = watches_.find(Str(s, "watch"));
    if (w == watches_.end()) {
      return "no watch " + Str(s, "watch");
    }
    const Commit &of = commits_[Str(s, "of")];
    Watch &watch = *w->second;
    std::unique_lock lock(watch.mu);
    const v2::Event *found = nullptr;
    watch.cv.wait_for(lock, std::chrono::seconds(5), [&] {
      for (const v2::Event &e : watch.events) {
        auto id = e.fields().find("request_id");
        if (id != e.fields().end() && id->second == of.request_id) {
          found = &e;
          return true;
        }
      }
      return false;
    });
    if (found == nullptr) {
      return "no event for " + of.request_id;
    }
    if (found->generation() != of.generation) {
      return std::format("event generation {}, commit's {}", found->generation(), of.generation);
    }
    if (const gp::Value *fields = Field(s, "want_fields")) {
      for (const auto &[k, v] : fields->struct_value().fields()) {
        auto f = found->fields().find(k);
        if (f == found->fields().end() || f->second != v.string_value()) {
          return "event " + found->ShortDebugString() + ": want " + k + "=" + v.string_value();
        }
      }
    }
    return std::nullopt;
  }

  Outcome DoCommit(const gp::Struct &s, const std::string &want_error) {
    auto r = client_.Resource(resource_);
    if (!r) {
      return "resource: " + r.error().message;
    }
    bess::sdk::TransactionOptions options;
    if (Field(s, "request_id_of") != nullptr) {
      options.request_id = commits_[Str(s, "request_id_of")].request_id;
    }
    if (Field(s, "expected_generation_of") != nullptr) {
      const gp::Value *delta = Field(s, "generation_delta");
      options.expected_generation = commits_[Str(s, "expected_generation_of")].generation +
                                    static_cast<int64_t>(delta ? delta->number_value() : 0);
    }
    options.snapshot = Bool(s, "snapshot");
    auto tx = client_.NewTransaction(options);
    int index = 0;
    for (const gp::Value *op_value : List(s, "ops")) {
      const gp::Struct &op = op_value->struct_value();
      std::string error;
      auto key = Make(Bool(op, "key_as_value") ? r->value_type : r->key_type, *Field(op, "key"),
                      &error);
      std::unique_ptr<gp::Message> value;
      if (key && Str(op, "op") == "upsert") {
        value = Make(r->value_type, *Field(op, "value"), &error);
      }
      if (!error.empty()) {
        return std::format("op {}: {}", index, error);
      }
      auto added = Str(op, "op") == "upsert" ? tx.Upsert(*r, *key, *value) : tx.Erase(*r, *key);
      if (Outcome failed = Expect(added ? nullptr : &added.error(), Str(op, "want_error"),
                                  std::format("op {}", index))) {
        return failed;
      }
      index++;
    }
    auto applied = tx.Commit();
    if (Outcome failed = Expect(applied ? nullptr : &applied.error(), want_error, "commit")) {
      return failed;
    }
    if (!applied) {
      if (Field(s, "want_failed_ops") != nullptr) {
        if (applied.error().kind != ErrorKind::kRejected) {
          return "want_failed_ops needs a rejected error, got " + KindName(applied.error().kind);
        }
        std::vector<int> failed;
        for (const auto &f : applied.error().failures) {
          failed.push_back(f.index);
        }
        std::vector<int> want;
        for (const gp::Value *i : List(s, "want_failed_ops")) {
          want.push_back(static_cast<int>(i->number_value()));
        }
        if (failed != want) {
          return "failed operations differ: " + applied.error().message;
        }
      }
      return std::nullopt;
    }
    if (Field(s, "want_replayed") != nullptr && applied->replayed != Bool(s, "want_replayed")) {
      return std::format("replayed = {}", applied->replayed);
    }
    if (Field(s, "want_generation_of") != nullptr &&
        applied->generation != commits_[Str(s, "want_generation_of")].generation) {
      return std::format("generation {}", applied->generation);
    }
    if (Field(s, "want_visibility") != nullptr &&
        v2::TransactionRecord_Visibility_Name(applied->visibility) !=
            Str(s, "want_visibility")) {
      return "visibility " + v2::TransactionRecord_Visibility_Name(applied->visibility);
    }
    if (Field(s, "as") != nullptr) {
      commits_[Str(s, "as")] = {applied->request_id, applied->generation};
    }
    return std::nullopt;
  }

  Outcome DoPipeline(const gp::Struct &s, const std::string &want_error) {
    const bool builds = Field(s, "add") || Field(s, "chain") || Field(s, "remove");
    v2::Pipeline base;
    if (builds && Field(s, "base") != nullptr) {
      base = pipes_[Str(s, "base")].pipeline;
    } else {
      auto snap = client_.Pipeline();
      if (!snap) {
        return Expect(&snap.error(), want_error, "pipeline");
      }
      if (!builds) {
        pipes_[Str(s, "as")] = {snap->pipeline, snap->version};
        return std::nullopt;
      }
      base = snap->pipeline;
    }
    bess::sdk::PipelineBuilder b(std::move(base));
    for (const gp::Value *m : List(s, "add")) {
      const auto &pair = m->list_value().values();
      b.Module(pair[0].string_value(), pair[1].string_value(), nullptr);
    }
    const auto chain = List(s, "chain");
    for (size_t i = 1; i < chain.size(); i++) {  // Chain: gate 0 to gate 0
      b.Connect(chain[i - 1]->string_value(), 0, chain[i]->string_value(), 0);
    }
    for (const gp::Value *m : List(s, "remove")) {
      b.Remove(m->string_value());
    }
    auto p = b.Build();
    if (!p) {
      return Expect(&p.error(), want_error, "build");
    }
    if (Bool(s, "plan")) {
      auto plan = client_.PlanPipeline(*p);
      if (!plan || plan->steps.empty()) {
        return "plan: " + (plan ? std::string("no steps") : plan.error().message);
      }
      if (Field(s, "want_plan_version_of") != nullptr &&
          plan->version != pipes_[Str(s, "want_plan_version_of")].version) {
        return std::format("plan against generation {} of epoch {}", plan->version.generation,
                           plan->version.daemon_epoch);
      }
    }
    if (const gp::Value *changes = Field(s, "want_changes")) {
      auto diff = client_.DiffPipeline(*p);
      if (!diff) {
        return "diff: " + diff.error().message;
      }
      if (diff->diff.changes_size() != static_cast<int>(changes->number_value())) {
        return std::format("{} change(s)", diff->diff.changes_size());
      }
    }
    std::optional<bess::sdk::PipelineVersion> at;
    if (Field(s, "at_version_of") != nullptr) {
      at = pipes_[Str(s, "at_version_of")].version;
    }
    auto done = client_.ApplyPipeline(*p, at);
    if (Outcome failed = Expect(done ? nullptr : &done.error(), want_error, "apply")) {
      return failed;
    }
    if (done && Field(s, "as") != nullptr) {
      pipes_[Str(s, "as")] = {*p, {done->generation(), done->daemon_epoch()}};
    }
    return std::nullopt;
  }

  Client &client_;
  std::string resource_;
  v2::GetSchemaResponse schema_;
  gp::DescriptorPool pool_;
  gp::DynamicMessageFactory factory_{&pool_};
  std::map<std::string, Commit> commits_;
  std::map<std::string, Pipe> pipes_;
  std::map<std::string, std::unique_ptr<Watch>> watches_;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s address resource cases.json\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[3]);
  const std::string text(std::istreambuf_iterator<char>(in), {});
  gp::Struct cases;
  if (auto status = gp::util::JsonStringToMessage(text, &cases); !in || !status.ok()) {
    std::fprintf(stderr, "%s: %s\n", argv[3], std::string(status.message()).c_str());
    return 2;
  }
  auto client = Client::Dial(argv[1]);
  Runner runner(*client, argv[2]);
  if (Outcome failed = runner.LoadSchema()) {
    std::fprintf(stderr, "%s\n", failed->c_str());
    return 1;
  }
  int failures = 0;
  for (const gp::Value *c : List(cases, "cases")) {
    const std::string name = Str(c->struct_value(), "name");
    if (Outcome failed = runner.RunCase(c->struct_value())) {
      std::printf("FAIL %s: %s\n", name.c_str(), failed->c_str());
      failures++;
    } else {
      std::printf("PASS %s\n", name.c_str());
    }
    std::fflush(stdout);
  }
  return failures == 0 ? 0 : 1;
}
