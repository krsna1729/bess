// SPDX-License-Identifier: BSD-3-Clause

// The C++ SDK against a live bessd (bessctl/module_tests/control_sdk_cpp.py
// starts it and passes the arguments): discovery, a typed commit and its
// event, a replay under the same request id, a conflict, a client-side type
// refusal, and a pipeline planned, applied and refused at a stale generation.
// The module's messages are built at run time from the daemon's own schema
// (GetSchema, DynamicMessageFactory), as a client without generated types
// would. The same steps as sdk/go/bess/live_test.go.
//
//   sdk_cpp_live_test <address> <resource> <key> <value>
//
// <key> and <value> are files holding the serialized messages. Exit 0: pass.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "bess/sdk/client.h"

namespace {

using bess::sdk::ErrorKind;
namespace v2 = ::bess::pb::v2;
namespace gp = google::protobuf;

int failures = 0;

#define CHECK_THAT(cond, ...)                         \
  do {                                                \
    if (!(cond)) {                                    \
      std::fprintf(stderr, "FAIL line %d: ", __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);              \
      std::fprintf(stderr, "\n");                     \
      return 1;                                       \
    }                                                 \
  } while (0)

std::string ReadFile(const char *path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

int main(int argc, char **argv) {
  CHECK_THAT(argc == 5, "usage: %s address resource key value", argv[0]);
  auto client = bess::sdk::Client::Dial(argv[1]);
  auto schema = client->Schema();
  CHECK_THAT(schema, "schema: %s", schema.error().message.c_str());
  gp::DescriptorPool pool;
  for (const auto &file : schema->descriptors().file()) {
    CHECK_THAT(pool.BuildFile(file) != nullptr, "cannot build %s", file.name().c_str());
  }
  gp::DynamicMessageFactory factory(&pool);
  std::string init_type;
  for (const auto &c : schema->module_classes()) {
    if (c.name() == "ExactMatch") {
      init_type = c.init_arg_type();
    }
  }
  CHECK_THAT(pool.FindMessageTypeByName(init_type) != nullptr, "ExactMatch init type '%s'",
             init_type.c_str());

  auto r = client->Resource(argv[2]);
  CHECK_THAT(r, "resource: %s", r.error().message.c_str());

  auto caps = client->Capabilities();
  CHECK_THAT(caps && !caps->daemon_version().empty() && caps->module_classes_size() > 0,
             "capabilities");
  auto supports = client->Supports("ApplyTransaction");
  CHECK_THAT(supports && *supports, "supports ApplyTransaction");
  auto samples = client->Metrics();
  CHECK_THAT(samples && !samples->empty(), "metrics");

  auto make = [&](const std::string &type, const char *file) -> std::unique_ptr<gp::Message> {
    const gp::Descriptor *d = pool.FindMessageTypeByName(type);
    if (d == nullptr) {
      return nullptr;
    }
    std::unique_ptr<gp::Message> m(factory.GetPrototype(d)->New());
    return m->ParseFromString(ReadFile(file)) ? std::move(m) : nullptr;
  };
  auto key = make(r->key_type, argv[3]);
  auto value = make(r->value_type, argv[4]);
  CHECK_THAT(key && value, "key %s / value %s", r->key_type.c_str(), r->value_type.c_str());

  // The event stream is open before the transaction.
  std::mutex mu;
  std::condition_variable cv;
  std::optional<v2::Event> event;
  std::jthread watcher([&](std::stop_token stop) {
    (void)client->WatchEvents(0, 0, {"bess.transaction"},
                              [&](const v2::Event &e) {
                                std::lock_guard lock(mu);
                                event = e;
                                cv.notify_all();
                                return false;
                              },
                              stop);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  auto tx = client->NewTransaction();
  CHECK_THAT(!tx.Upsert(*r, *value, *value), "a value as key was accepted");
  auto added = tx.Upsert(*r, *key, *value);
  CHECK_THAT(added, "upsert: %s", added.error().message.c_str());
  auto first = tx.Commit();
  CHECK_THAT(first, "commit: %s", first.error().message.c_str());

  {
    std::unique_lock lock(mu);
    CHECK_THAT(cv.wait_for(lock, std::chrono::seconds(5), [&] { return event.has_value(); }),
               "no transaction event");
    const auto &f = event->fields();
    CHECK_THAT(f.count("outcome") == 1 && f.at("outcome") == "applied" &&
                   event->generation() == first->generation &&
                   f.count("request_id") == 1 && f.at("request_id") == tx.request_id(),
               "transaction event %s", event->ShortDebugString().c_str());
  }

  auto again = client->NewTransaction({.request_id = tx.request_id()});
  CHECK_THAT(again.Upsert(*r, *key, *value), "upsert again");
  auto replay = again.Commit();
  CHECK_THAT(replay && replay->replayed && replay->generation == first->generation, "replay");

  auto stale = client->NewTransaction({.expected_generation = first->generation - 1});
  CHECK_THAT(stale.Upsert(*r, *key, *value), "upsert stale");
  auto refused = stale.Commit();
  CHECK_THAT(!refused && refused.error().kind == ErrorKind::kConflict,
             "a stale generation applied or was not a conflict");

  // Desired state: two modules added to the active pipeline, planned and
  // applied at its generation; then refused at that (stale) generation.
  auto snap = client->Pipeline();
  CHECK_THAT(snap, "pipeline: %s", snap.error().message.c_str());
  auto with = bess::sdk::PipelineBuilder(snap->pipeline)
                  .Module("cpp_sdk_a", "Bypass", nullptr)
                  .Module("cpp_sdk_b", "Sink", nullptr)
                  .Chain({"cpp_sdk_a", "cpp_sdk_b"})
                  .Build();
  CHECK_THAT(with, "build");
  auto plan = client->PlanPipeline(*with);
  CHECK_THAT(plan && plan->generation == snap->generation && !plan->steps.empty(), "plan");
  auto done = client->ApplyPipeline(*with, snap->generation);
  CHECK_THAT(done && done->generation() > snap->generation, "apply: %s",
             done ? "" : done.error().message.c_str());
  auto without =
      bess::sdk::PipelineBuilder(*with).Remove("cpp_sdk_a").Remove("cpp_sdk_b").Build();
  auto conflict = client->ApplyPipeline(*without, snap->generation);
  CHECK_THAT(!conflict && conflict.error().kind == ErrorKind::kPipelineConflict,
             "a stale pipeline generation applied or was not a conflict");
  auto restored = client->ApplyPipeline(*without, done->generation());
  CHECK_THAT(restored, "restore: %s", restored ? "" : restored.error().message.c_str());

  std::printf("ok\n");
  return failures;
}
