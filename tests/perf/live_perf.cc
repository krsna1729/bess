// SPDX-License-Identifier: BSD-3-Clause

// Classified live performance tests: which part of BESS got slower (D-092,
// D-138). Each test is a small pipeline on one pinned worker that isolates one
// cost; the result is ns per packet from the measured modules' output-gate
// counters (the default Track hook every connection has) over a timed window.
//
//   F  framework, cross-cutting -- every pipeline pays it:
//        F.floor        Source -> Sink
//        F.hop/N        Source -> N x Bypass -> Sink      (slope: cost per module hop)
//        F.hopnt/16     F.hop/16 without the default Track hook on the Bypasses' gates
//                       (F.hop/16 - F.hopnt/16 = 16 gates' Track cost)
//        F.fanout/N     Source -> RoundRobin(N gates) -> N Sinks
//        F.split/N      F.fanout/N with packets alternating gates inside each burst
//        F.fanin/N      N x Source -> Merge -> one Sink
//        F.tc/N         N Sources under one round_robin traffic class (scheduler cost)
//        F.size/B       F.floor with B-byte packets (allocation and copy)
//        F.queue        Source -> Queue | worker 1: Queue -> Sink (cross-worker handoff)
//   M  module, localised -- ns per packet above M.base (Source -> Rewrite ->
//        RandomUpdate -> Bypass -> Sink, the headers and flow mix every M test uses).
//
// Runs: one or more builds, palindromic rounds (A B ... B A), each round a
// fresh bessd (sudo -n) running every selected test, under an isolation tool
// such as `omarchy-benchmark --isolate`:
//
//   live_perf --build current --build other=/tree/build/x/core/bessd --rounds 4 --cpu 2,4
//
// --save keeps medians and raw values (JSON); --baseline FILE --threshold P
// compares the last build against a saved run and exits 1 when a test is
// slower by more than P percent.

#include <fcntl.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "bess/sdk/client.h"

extern char **environ;

namespace {

namespace fs = std::filesystem;
namespace gp = google::protobuf;
namespace v2 = bess::pb::v2;
using Clock = std::chrono::steady_clock;

constexpr int kFlows = 1024;  // source addresses the M tests cycle through
constexpr const char *kCounters = "instructions,cycles,L1-dcache-load-misses,branch-misses";

// -- packets ---------------------------------------------------------------------

// An Ethernet/IPv4/UDP frame of `size` bytes, IPv4 checksum filled. The
// source port is unprivileged: NAT maps a port below 1024 only into a range
// below 1024, and M.nat's range starts at 1024.
std::string Ipv4Udp(uint32_t src = 0x0a000001, uint32_t dst = 0x0b000001, uint16_t sport = 40000,
                    uint16_t dport = 2000, size_t size = 60) {
  std::string f = {0x02, 0, 0, 0, 0, 0x02, 0x02, 0, 0, 0, 0, 0x01, 0x08, 0x00};
  const size_t payload = size > f.size() + 28 ? size - f.size() - 28 : 0;
  const size_t total = 28 + payload;
  uint8_t ip[20] = {0x45, 0, uint8_t(total >> 8), uint8_t(total), 0, 0, 0, 0, 64, 17, 0, 0,
                    uint8_t(src >> 24), uint8_t(src >> 16), uint8_t(src >> 8), uint8_t(src),
                    uint8_t(dst >> 24), uint8_t(dst >> 16), uint8_t(dst >> 8), uint8_t(dst)};
  uint32_t sum = 0;
  for (int i = 0; i < 20; i += 2) {
    sum += (ip[i] << 8) | ip[i + 1];
  }
  sum = (sum & 0xffff) + (sum >> 16);
  sum = ~((sum & 0xffff) + (sum >> 16)) & 0xffff;
  ip[10] = sum >> 8;
  ip[11] = sum & 0xff;
  f.append(reinterpret_cast<const char *>(ip), 20);
  const size_t udp_len = 8 + payload;
  const uint8_t udp[8] = {uint8_t(sport >> 8), uint8_t(sport), uint8_t(dport >> 8), uint8_t(dport),
                          uint8_t(udp_len >> 8), uint8_t(udp_len), 0, 0};
  f.append(reinterpret_cast<const char *>(udp), 8);
  f.append(payload, '\0');
  return f;
}

std::string Base64(const std::string &in) {
  static constexpr char kDigits[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < in.size(); i += 3) {
    const auto at = [&](size_t j) { return j < in.size() ? uint8_t(in[j]) : 0u; };
    const uint32_t n = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
    out += kDigits[(n >> 18) & 63];
    out += kDigits[(n >> 12) & 63];
    out += i + 1 < in.size() ? kDigits[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kDigits[n & 63] : '=';
  }
  return out;
}

std::string Be32(uint32_t v) {
  return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
}

// -- the daemon's types, from GetSchema ----------------------------------------------

class Types {
 public:
  explicit Types(bess::sdk::Client &client) {
    auto schema = client.Schema();
    if (!schema) {
      throw std::runtime_error("GetSchema: " + schema.error().message);
    }
    for (const gp::FileDescriptorProto &file : schema->descriptors().file()) {
      pool_.BuildFile(file);
    }
    for (const v2::ModuleClassSchema &c : schema->module_classes()) {
      init_types_[c.name()] = c.init_arg_type();
    }
  }
  // A message of `type` (a full name, or one in bess.pb) from protobuf JSON.
  std::unique_ptr<gp::Message> Make(const std::string &type, const std::string &json) {
    const std::string full = type.find('.') == std::string::npos ? "bess.pb." + type : type;
    const gp::Descriptor *d = pool_.FindMessageTypeByName(full);
    if (d == nullptr) {
      throw std::runtime_error("the daemon has no type " + full);
    }
    std::unique_ptr<gp::Message> m(factory_.GetPrototype(d)->New());
    if (auto ok = gp::util::JsonStringToMessage(json, m.get()); !ok.ok()) {
      throw std::runtime_error(full + " " + json + ": " + std::string(ok.message()));
    }
    return m;
  }
  // The Init argument type of module class `mclass`.
  const std::string &InitType(const std::string &mclass) const {
    const auto it = init_types_.find(mclass);
    if (it == init_types_.end() || it->second.empty()) {
      throw std::runtime_error("no module class " + mclass + " with an Init argument type");
    }
    return it->second;
  }

 private:
  std::map<std::string, std::string> init_types_;
  gp::DescriptorPool pool_;
  gp::DynamicMessageFactory factory_{&pool_};
};

// -- the catalogue -----------------------------------------------------------------

// What a test builds: the pipeline, on worker 0 (and worker 1 where it says).
class Build {
 public:
  explicit Build(Types &types) : types_(types) {}

  void Mod(const std::string &mclass, const std::string &name, const std::string &json = "{}") {
    args_.push_back(types_.Make(types_.InitType(mclass), json));
    b_.Module(name, mclass, args_.back().get());
  }
  void Conn(const std::string &a, const std::string &b, uint32_t ogate = 0, uint32_t igate = 0) {
    b_.Connect(a, ogate, b, igate);
  }
  // The connection out of `module`'s gate 0 without the default Track hook.
  void NoTrack(const std::string &module) { no_track_.insert(module); }
  void Task(const std::string &module, std::optional<int> wid, const std::string &parent = "") {
    v2::TaskPlacement t;
    t.set_module(module);
    if (wid) {
      t.set_wid(*wid);
    }
    t.set_parent(parent);
    b_.Task(t);
  }
  void TrafficClass(const std::string &name, const std::string &policy, int wid) {
    v2::TrafficClass tc;
    tc.set_name(name);
    tc.set_policy(policy);
    tc.set_wid(wid);
    b_.TrafficClass(tc);
  }
  void Entry(const std::string &resource, const std::string &key_type, const std::string &key,
             const std::string &value_type, const std::string &value) {
    auto k = types_.Make(key_type, key);
    auto v = types_.Make(value_type, value);
    b_.Entry(resource, *k, *v);
  }
  void Setting(const std::string &resource, const std::string &type, const std::string &value) {
    b_.Setting(resource, *types_.Make(type, value));
  }
  void Worker(int wid, int core) { b_.Worker(wid, core, ""); }

  v2::Pipeline Pipeline() const {
    auto p = b_.Build();
    if (!p) {
      throw std::runtime_error("pipeline: " + p.error().message);
    }
    for (v2::Connection &c : *p->mutable_connections()) {
      if (no_track_.contains(c.upstream())) {
        c.set_skip_default_hooks(true);
      }
    }
    return *p;
  }

 private:
  Types &types_;
  bess::sdk::PipelineBuilder b_;
  std::vector<std::unique_ptr<gp::Message>> args_;
  std::set<std::string> no_track_;
};

using Builder = std::function<std::vector<std::string>(Build &)>;

struct Test {
  std::string name;
  char cls;
  Builder build;
  bool two_workers = false;
};

std::string Source(Build &b, const std::string &name = "src", int size = 60,
                   const std::optional<std::string> &frame = std::nullopt) {
  b.Mod("Source", name, R"({"pkt_size": )" + std::to_string(size) + "}");
  if (frame) {
    b.Mod("Rewrite", name + "_rw", R"({"templates": [")" + Base64(*frame) + R"("]})");
    b.Conn(name, name + "_rw");
    return name + "_rw";
  }
  return name;
}

std::vector<std::string> FFloor(Build &b, int size) {
  b.Mod("Sink", "sink");
  b.Conn(Source(b, "src", size), "sink");
  return {"src"};
}

Builder FHop(int n, bool track) {
  return [n, track](Build &b) {
    std::string last = Source(b);
    for (int i = 0; i < n; i++) {
      const std::string bp = "bp" + std::to_string(i);
      b.Mod("Bypass", bp);
      b.Conn(last, bp);
      last = bp;
      if (!track) {
        b.NoTrack(bp);
      }
    }
    b.Mod("Sink", "sink");
    b.Conn(last, "sink");
    return std::vector<std::string>{"src"};
  };
}

std::string Gates(int n) {
  std::string out = "[";
  for (int g = 0; g < n; g++) {
    out += (g ? ", " : "") + std::to_string(g);
  }
  return out + "]";
}

Builder FFanout(int n, bool per_packet) {
  return [n, per_packet](Build &b) {
    b.Mod("RoundRobin", "rr",
          R"({"gates": )" + Gates(n) + (per_packet ? R"(, "mode": "packet"})" : "}"));
    b.Conn(Source(b), "rr");
    for (int g = 0; g < n; g++) {
      b.Mod("Sink", "sink" + std::to_string(g));
      b.Conn("rr", "sink" + std::to_string(g), g);
    }
    return std::vector<std::string>{"src"};
  };
}

Builder FFanin(int n) {
  return [n](Build &b) {
    b.Mod("Merge", "merge");
    b.Mod("Sink", "sink");
    b.Conn("merge", "sink");
    std::vector<std::string> names;
    for (int i = 0; i < n; i++) {
      const std::string src = "src" + std::to_string(i);
      b.Mod("Source", src, R"({"pkt_size": 60})");
      b.Conn(src, "merge", 0, i);
      names.push_back(src);
    }
    return names;
  };
}

Builder FTc(int n) {
  return [n](Build &b) {
    b.TrafficClass("rr_root", "round_robin", 0);
    std::vector<std::string> names;
    for (int i = 0; i < n; i++) {
      const std::string src = "src" + std::to_string(i);
      b.Mod("Source", src, R"({"pkt_size": 60})");
      b.Mod("Sink", "sink" + std::to_string(i));
      b.Conn(src, "sink" + std::to_string(i));
      b.Task(src, std::nullopt, "rr_root");
      names.push_back(src);
    }
    return names;
  };
}

std::vector<std::string> FQueue(Build &b) {
  b.Mod("Queue", "q", R"({"size": 1024})");
  b.Mod("Sink", "sink");
  b.Conn(Source(b), "q");
  b.Conn("q", "sink");
  b.Task("src", 0);
  b.Task("q", 1);
  return {"q"};
}

// Source -> Rewrite -> RandomUpdate (kFlows source addresses) -> [rest] -> Sink;
// `rest`: (module, output gates) in order, created by the caller except Bypass.
std::vector<std::string> MBase(Build &b, std::vector<std::pair<std::string, int>> rest = {},
                               const std::optional<std::string> &frame = std::nullopt) {
  std::string last = Source(b, "src", 60, frame ? *frame : Ipv4Udp());
  b.Mod("RandomUpdate", "ru",
        R"({"fields": [{"offset": 26, "size": 4, "min": )" + std::to_string(0x0a000001) +
            R"(, "max": )" + std::to_string(0x0a000000 + kFlows) + "}]}");
  b.Conn(last, "ru");
  last = "ru";
  if (rest.empty()) {
    rest = {{"bp", 1}};
  }
  for (const auto &[name, gates] : rest) {
    if (name == "bp") {
      b.Mod("Bypass", "bp");
    }
    b.Conn(last, name);
    last = name;
    if (gates > 1) {
      for (int g = 0; g < gates; g++) {
        b.Mod("Sink", "sink" + std::to_string(g));
        b.Conn(name, "sink" + std::to_string(g), g);
      }
      return {"src"};
    }
  }
  b.Mod("Sink", "sink");
  b.Conn(last, "sink");
  return {"src"};
}

// ExactMatch on the source address: rules for addresses base + i, i in
// [0, kFlows) stepping `step`, to gate 1; the default gate 0.
Builder MExactMatch(uint32_t base, int step) {
  return [base, step](Build &b) {
    b.Mod("ExactMatch", "em", R"({"fields": [{"offset": 26, "num_bytes": 4}]})");
    b.Setting("em/settings", "ExactMatchSettings", R"({"default_gate": 0})");
    for (int i = 0; i < kFlows; i += step) {
      b.Entry("em/rules", "ExactMatchRuleKey",
              R"({"fields": [{"value_bin": ")" + Base64(Be32(base + i)) + R"("}]})",
              "ExactMatchRuleValue", R"({"gate": 1})");
    }
    return MBase(b, {{"em", 2}});
  };
}

std::string IntField(uint64_t v) {
  return R"({"value_int": )" + std::to_string(v) + "}";
}

void WmRule(Build &b, const std::vector<uint64_t> &values, const std::vector<uint64_t> &masks,
            int priority) {
  std::string vs, ms;
  for (size_t i = 0; i < values.size(); i++) {
    vs += (i ? ", " : "") + IntField(values[i]);
    ms += (i ? ", " : "") + IntField(masks[i]);
  }
  b.Entry("wm/rules", "WildcardMatchRuleKey", R"({"values": [)" + vs + R"(], "masks": [)" + ms + "]}",
          "WildcardMatchRuleValue",
          R"({"gate": 1, "priority": )" + std::to_string(priority) + "}");
}

constexpr uint32_t kWmMasks[] = {0xffffff00, 0xffff0000, 0xff000000, 0xfffff000};

std::vector<std::string> MWildcardMatch(Build &b) {
  b.Mod("WildcardMatch", "wm",
        R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}]})");
  b.Setting("wm/settings", "WildcardMatchSettings", R"({"default_gate": 0})");
  for (int i = 0; i < 4; i++) {
    WmRule(b, {0x0a000000 & kWmMasks[i], 0x0b000000}, {kWmMasks[i], 0xff000000}, i);
  }
  return MBase(b, {{"wm", 2}});
}

// The four masks on a 13-byte 5-tuple key (source, destination, protocol,
// source port wildcarded, destination port): 8-, 4- and 1-byte mask steps.
std::vector<std::string> MWildcardMatch13(Build &b) {
  b.Mod("WildcardMatch", "wm",
        R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4},)"
        R"( {"offset": 23, "num_bytes": 1}, {"offset": 34, "num_bytes": 2},)"
        R"( {"offset": 36, "num_bytes": 2}]})");
  b.Setting("wm/settings", "WildcardMatchSettings", R"({"default_gate": 0})");
  for (int i = 0; i < 4; i++) {
    WmRule(b, {0x0a000000 & kWmMasks[i], 0x0b000000, 17, 0, 2000},
           {kWmMasks[i], 0xff000000, 0xff, 0, 0xffff}, i);
  }
  return MBase(b, {{"wm", 2}});
}

// n masks in use (/8 .. /8+n-1), all at one priority: every mask is looked
// up (the worst case). Above 8 needs max_tuples.
Builder MWildcardMasks(int n) {
  return [n](Build &b) {
    b.Mod("WildcardMatch", "wm",
          R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}])" +
              (n > 8 ? R"(, "max_tuples": )" + std::to_string(n) : std::string()) + "}");
    b.Setting("wm/settings", "WildcardMatchSettings", R"({"default_gate": 0})");
    for (int i = 0; i < n; i++) {
      const uint32_t mask = uint32_t(0xffffffffull << (32 - (8 + i)));
      WmRule(b, {0x0a000000 & mask, 0x0b000000}, {mask, 0xff000000}, 0);
    }
    return MBase(b, {{"wm", 2}});
  };
}

// The session slice: ExactMatch (action mode, one rule per flow) ->
// ActionTable -> Meter (never red) -> Router (one route).
std::vector<std::string> MSession(Build &b) {
  b.Mod("ActionTable", "at",
        R"({"capacity": 64, "meters": "mt/meters", "next_hops": "rt/next_hops"})");
  b.Mod("ExactMatch", "em",
        R"({"fields": [{"offset": 26, "num_bytes": 4}], "action_resource": "at/actions"})");
  b.Mod("Meter", "mt", R"({"capacity": 64})");
  b.Mod("Router", "rt", R"({"max_routes": 64, "max_tbl8s": 16, "max_next_hops": 64})");
  b.Entry("mt/meters", "MeterIdKey", R"({"id": 1})", "MeterPolicyValue",
          R"({"tr_tcm": {"committed_rate": "1000000000000", "committed_burst": "1000000000",)"
          R"( "peak_rate": "1000000000000", "peak_burst": "1000000000"}})");
  b.Entry("rt/next_hops", "RouterNextHopIdKey", R"({"id": 1})", "RouterNextHopValue",
          R"({"egress_gate": 0})");
  b.Entry("rt/routes", "RouterRouteKey", R"({"ipv4": "11.0.0.0", "prefix_length": 8})",
          "RouterRouteValue", R"({"next_hop_id": 1})");
  b.Entry("at/actions", "ActionIdKey", R"({"id": 1})", "ActionValue",
          R"({"meter_id": 1, "next_hop_id": 1})");
  for (int i = 0; i < kFlows; i++) {
    b.Entry("em/rules", "ExactMatchRuleKey",
            R"({"fields": [{"value_bin": ")" + Base64(Be32(0x0a000001 + i)) + R"("}]})",
            "ExactMatchRuleValue", R"({"action_id": 1})");
  }
  return MBase(b, {{"em", 1}, {"at", 1}, {"mt", 1}, {"rt", 1}});
}

Builder MIpLookup(const std::string &arg) {
  return [arg](Build &b) {
    b.Mod("IPLookup", "ipl", arg);
    for (int i = 0; i < 256; i++) {
      b.Entry("ipl/routes", "IPLookupRouteKey",
              R"({"prefix": "11.)" + std::to_string(i) + R"(.0.0", "prefix_len": 16})",
              "IPLookupRouteValue", R"({"gate": 1})");
    }
    b.Entry("ipl/routes", "IPLookupRouteKey", R"({"prefix": "0.0.0.0", "prefix_len": 0})",
            "IPLookupRouteValue", R"({"gate": 0})");
    return MBase(b, {{"ipl", 2}});
  };
}

std::vector<std::string> ML2Forward(Build &b) {
  b.Mod("L2Forward", "l2");
  b.Entry("l2/macs", "L2ForwardMacKey", R"({"addr": "02:00:00:00:00:02"})", "L2ForwardMacValue",
          R"({"gate": 1})");
  b.Setting("l2/settings", "L2ForwardSettings", R"({"default_gate": 0})");
  return MBase(b, {{"l2", 2}});
}

std::vector<std::string> MAcl(Build &b) {
  std::string rules;
  for (int i = 0; i < 16; i++) {
    rules += std::string(i ? ", " : "") + R"({"src_ip": "10.0.)" + std::to_string(i) +
             R"(.0/24", "dst_ip": "11.0.0.0/8", "drop": false})";
  }
  b.Mod("ACL", "acl", R"({"rules": [)" + rules + "]}");
  return MBase(b, {{"acl", 1}});
}

std::vector<Test> Catalogue() {
  const auto m = [](const std::string &mclass, const std::string &name, const std::string &arg,
                    int gates) -> Builder {
    return [=](Build &b) {
      b.Mod(mclass, name, arg);
      return MBase(b, {{name, gates}});
    };
  };
  return {
      {"F.floor", 'F', [](Build &b) { return FFloor(b, 60); }},
      {"F.hop/1", 'F', FHop(1, true)},
      {"F.hop/4", 'F', FHop(4, true)},
      {"F.hop/16", 'F', FHop(16, true)},
      {"F.hopnt/16", 'F', FHop(16, false)},
      {"F.fanout/4", 'F', FFanout(4, false)},
      {"F.split/2", 'F', FFanout(2, true)},
      {"F.fanin/4", 'F', FFanin(4)},
      {"F.tc/1", 'F', FTc(1)},
      {"F.tc/16", 'F', FTc(16)},
      {"F.size/1500", 'F', [](Build &b) { return FFloor(b, 1500); }},
      {"F.queue", 'F', FQueue, true},
      {"M.base", 'M', [](Build &b) { return MBase(b); }},
      {"M.exactmatch", 'M', MExactMatch(0x0a000001, 1)},
      // Every other flow matches: bursts split across two gates.
      {"M.exactmatch/mixed", 'M', MExactMatch(0x0a000001, 2)},
      // Every flow misses (the rules hold other addresses).
      {"M.exactmatch/miss", 'M', MExactMatch(0x0b000001, 1)},
      // Bursts split across 2 gates packet by packet: the emit cost alone.
      {"M.split/2", 'M', m("RoundRobin", "rr", R"({"gates": [0, 1], "mode": "packet"})", 2)},
      {"M.wildcardmatch", 'M', MWildcardMatch},
      {"M.wildcardmatch/13b", 'M', MWildcardMatch13},
      {"M.wildcardmatch/8", 'M', MWildcardMasks(8)},
      {"M.wildcardmatch/16", 'M', MWildcardMasks(16)},
      {"M.session", 'M', MSession},
      {"M.iplookup", 'M', MIpLookup("{}")},
      {"M.iplookup/fib", 'M', MIpLookup(R"({"backend": "ROUTE_BACKEND_FIB"})")},
      {"M.l2forward", 'M', ML2Forward},
      {"M.acl", 'M', MAcl},
      {"M.hashlb", 'M', m("HashLB", "lb", R"({"gates": [0, 1, 2, 3], "mode": "l3"})", 4)},
      {"M.update", 'M',
       m("Update", "up",
         R"({"fields": [{"offset": 22, "size": 1, "value": 32},)"
         R"( {"offset": 0, "size": 6, "value": )" +
             std::to_string(0x020000000003ull) + "}]}",
         1)},
      {"M.nat", 'M',
       m("NAT", "nat",
         R"({"ext_addrs": [{"ext_addr": "192.0.2.1", "port_ranges": [{"begin": 1024, "end": 65535}]}]})",
         2)},
      {"M.vlan", 'M',
       [](Build &b) {
         b.Mod("VLANPush", "vpush", R"({"tci": 7})");
         b.Mod("VLANPop", "vpop");
         return MBase(b, {{"vpush", 1}, {"vpop", 1}});
       }},
      // A 14-byte header of constants only.
      {"M.genericencap", 'M',
       m("GenericEncap", "ge",
         R"({"fields": [{"size": 6, "value": {"value_int": )" + std::to_string(0x020000000002ull) +
             R"(}}, {"size": 6, "value": {"value_int": )" + std::to_string(0x020000000001ull) +
             R"(}}, {"size": 2, "value": {"value_int": 2048}}]})",
         1)},
      // Timestamp and SetMetadata write attributes GenericEncap reads.
      {"M.metadata", 'M',
       [](Build &b) {
         b.Mod("Timestamp", "ts", R"({"attr_name": "ts"})");
         b.Mod("SetMetadata", "sm", R"({"attrs": [{"name": "tag", "size": 4, "value_int": 7}]})");
         b.Mod("GenericEncap", "ge",
               R"({"fields": [{"size": 8, "attribute": "ts"}, {"size": 4, "attribute": "tag"},)"
               R"( {"size": 2, "value": {"value_int": 2048}}]})");
         return MBase(b, {{"ts", 1}, {"sm", 1}, {"ge", 1}});
       }},
      // One compiled filter that every packet matches.
      {"M.bpf", 'M',
       m("BPF", "bpf",
         R"({"filters": [{"priority": 0, "filter": "udp dst port 2000", "gate": 1}]})", 2)},
  };
}

// -- one round: a fresh bessd, every test ------------------------------------------

struct Options {
  std::vector<std::string> tests;
  std::vector<int> cpus = {2, 4};
  std::optional<int> control_cpu;
  double duration = 1.0;
  double warmup = 0.3;
  bool counters = false;
  int rounds = 3;
  std::string save;
  std::string baseline;
  double threshold = 5.0;
};

struct BuildSpec {
  std::string name;
  std::string bessd;
  std::vector<std::string> flags;
};

// A test's outcome: ns per packet and counters per packet, or an error.
struct Sample {
  std::optional<double> ns;
  std::map<std::string, double> counters;
  std::string error;
};

int FreePort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
  close(fd);
  return ntohs(a.sin_port);
}

bool Listening(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  const bool ok = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
  close(fd);
  return ok;
}

pid_t Spawn(const std::vector<std::string> &argv, int out_fd = -1) {
  std::vector<char *> args;
  for (const std::string &a : argv) {
    args.push_back(const_cast<char *>(a.c_str()));
  }
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  const int null_fd = open("/dev/null", O_WRONLY);
  posix_spawn_file_actions_adddup2(&actions, out_fd >= 0 ? out_fd : null_fd, 1);
  posix_spawn_file_actions_adddup2(&actions, out_fd >= 0 ? out_fd : null_fd, 2);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(null_fd);
  if (rc != 0) {
    throw std::runtime_error("cannot run " + argv[0]);
  }
  return pid;
}

int Run(const std::vector<std::string> &argv) {
  int status = 0;
  waitpid(Spawn(argv), &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Packets that left `modules` by any output gate.
double OutPackets(bess::sdk::Client &client, const std::vector<std::string> &modules) {
  auto samples = client.Metrics();
  if (!samples) {
    throw std::runtime_error("metrics: " + samples.error().message);
  }
  double total = 0;
  for (const v2::MetricSample &s : *samples) {
    const auto &l = s.labels();
    if (s.name() == "bess_gate_packets_total" && l.contains("direction") &&
        l.at("direction") == "out" && l.contains("module") &&
        std::find(modules.begin(), modules.end(), l.at("module")) != modules.end()) {
      total += s.value();
    }
  }
  return total;
}

void Check(const bess::sdk::Result<void> &r, const std::string &what) {
  if (!r) {
    throw std::runtime_error(what + ": " + r.error().message);
  }
}

Sample RunTest(bess::sdk::Client &client, Types &types, const Test &test, const Options &o) {
  Sample out;
  try {
    Check(client.PauseWorkers(), "pause");
    auto cleared = client.ApplyPipeline(v2::Pipeline());
    if (!cleared) {
      throw std::runtime_error("reset: " + cleared.error().message);
    }
    Build b(types);
    b.Worker(0, o.cpus[0]);
    // Worker 1 only where a test places a task on it.
    if (test.two_workers && o.cpus.size() > 1) {
      b.Worker(1, o.cpus[1]);
    }
    const std::vector<std::string> modules = test.build(b);
    auto applied = client.ApplyPipeline(b.Pipeline());
    if (!applied) {
      throw std::runtime_error(applied.error().message);
    }
    Check(client.ResumeWorkers(), "resume");
    std::this_thread::sleep_for(std::chrono::duration<double>(o.warmup));
    // Hardware counters on worker 0's CPU over the window: per packet, more
    // instructions is more code; more cycles at equal instructions is stalls.
    pid_t perf = -1;
    const std::string perf_out = "/tmp/live_perf_counters_" + std::to_string(getpid());
    if (o.counters) {
      perf = Spawn({"perf", "stat", "-x", ",", "-o", perf_out, "-C", std::to_string(o.cpus[0]),
                    "-e", kCounters, "sleep", std::to_string(o.duration)});
    }
    const auto t0 = Clock::now();
    const double p0 = OutPackets(client, modules);
    std::this_thread::sleep_for(std::chrono::duration<double>(o.duration));
    const auto t1 = Clock::now();
    const double p1 = OutPackets(client, modules);
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    if (p1 > p0) {
      out.ns = seconds * 1e9 / (p1 - p0);
    }
    if (perf > 0) {
      int status = 0;
      waitpid(perf, &status, 0);
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        static bool warned = false;
        if (!warned) {
          std::fprintf(stderr,
                       "live_perf: perf stat failed; no counters (needs perf and "
                       "kernel.perf_event_paranoid <= 0)\n");
          warned = true;
        }
      }
      std::ifstream in(perf_out);
      std::string line;
      while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string x; std::getline(ss, x, ',');) {
          f.push_back(x);
        }
        if (f.size() > 2 && !f[0].empty() && std::isdigit(static_cast<unsigned char>(f[0][0]))) {
          // 'instructions', or 'cpu_core/instructions/' on hybrid CPUs.
          std::string event = f[2];
          if (auto slash = event.find('/'); slash != std::string::npos) {
            event = event.substr(slash + 1, event.find('/', slash + 1) - slash - 1);
          } else {
            event = event.substr(0, event.find(':'));
          }
          out.counters[event + "_per_pkt"] += std::stod(f[0]) / std::max(1.0, p1 - p0);
        }
      }
      fs::remove(perf_out);
    }
  } catch (const std::exception &e) {
    // Recorded, not fatal: a test this build cannot run.
    std::string what = e.what();
    out.error = "error: " + what.substr(0, std::min<size_t>(160, what.find('\n')));
  }
  return out;
}

std::map<std::string, Sample> RunRound(const BuildSpec &spec, const std::vector<Test> &tests,
                                       const Options &o) {
  const int port = FreePort();
  const std::string pidfile = "/tmp/live_perf_" + std::to_string(port) + ".pid";
  std::vector<std::string> cmd = {"sudo", "-n", "env"};
  if (const char *ld = std::getenv("BESS_DPDK_LIB")) {
    cmd.push_back(std::string("LD_LIBRARY_PATH=") + ld);
  } else if (const char *ld2 = std::getenv("LD_LIBRARY_PATH")) {
    cmd.push_back(std::string("LD_LIBRARY_PATH=") + ld2);
  }
  cmd.insert(cmd.end(), {spec.bessd, "-f", "-p", std::to_string(port), "--skip_root_check", "-m",
                         "0", "-i", pidfile, "-c", std::to_string(o.cpus.back())});
  cmd.insert(cmd.end(), spec.flags.begin(), spec.flags.end());
  const pid_t daemon = Spawn(cmd);
  std::map<std::string, Sample> out;
  try {
    for (int i = 0; i < 120 && !Listening(port); i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (o.control_cpu) {
      // Every daemon thread not pinned to one CPU (main, gRPC, DPDK service
      // threads) onto the control CPU; workers pin themselves.
      std::ifstream pf(pidfile);
      long pid = 0;
      if (!(pf >> pid) || pid <= 0) {
        throw std::runtime_error("no daemon pid in " + pidfile);
      }
      for (const auto &task : fs::directory_iterator("/proc/" + std::to_string(pid) + "/task")) {
        std::ifstream status(task.path() / "status");
        std::string line;
        while (std::getline(status, line)) {
          if (line.starts_with("Cpus_allowed_list:")) {
            const std::string allowed = line.substr(line.find_first_not_of(" \t", 18));
            if (allowed.find_first_of(",-") != std::string::npos) {
              Run({"sudo", "-n", "taskset", "-p", "-c", std::to_string(*o.control_cpu),
                   task.path().filename().string()});
            }
          }
        }
      }
    }
    auto client = bess::sdk::Client::Dial("localhost:" + std::to_string(port));
    Types types(*client);
    for (const Test &t : tests) {
      out[t.name] = RunTest(*client, types, t, o);
    }
    (void)client->PauseWorkers();
    (void)client->ApplyPipeline(v2::Pipeline());
  } catch (const std::exception &e) {
    for (const Test &t : tests) {
      out[t.name].error = std::string("error: ") + e.what();
    }
  }
  const std::string pattern = "bessd .* -p " + std::to_string(port) + " ";
  Run({"sudo", "-n", "pkill", "-f", pattern});
  // Up to 10 s for a clean exit, then SIGKILL: a bessd stuck in shutdown
  // must not hang the run.
  int status = 0;
  for (int i = 0; i < 100 && waitpid(daemon, &status, WNOHANG) == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (i == 99) {
      Run({"sudo", "-n", "pkill", "-KILL", "-f", pattern});
      waitpid(daemon, &status, 0);
    }
  }
  Run({"sudo", "-n", "rm", "-f", pidfile});
  return out;
}

// -- builds, report, save, baseline --------------------------------------------------

// 'current' (the bessd of this build directory), 'current:<tree>[:<builddir>
// [:<bessd flags, comma-separated>]]' (builddir default build/perf-release;
// the same binary under other flags is a build of its own), or
// 'name=<bessd>[,flag...]'.
BuildSpec ParseBuild(const std::string &text) {
  if (text == "current") {
    const fs::path self = fs::read_symlink("/proc/self/exe");
    return {"current", (self.parent_path().parent_path().parent_path() / "core" / "bessd").string(),
            {}};
  }
  if (text.starts_with("current:")) {
    std::vector<std::string> parts;
    std::stringstream ss(text);
    for (std::string p; parts.size() < 3 && std::getline(ss, p, ':');) {
      parts.push_back(p);
    }
    std::string flags;
    std::getline(ss, flags);
    const fs::path tree = parts.size() > 1 ? parts[1] : ".";
    const std::string build = parts.size() > 2 ? parts[2] : "build/perf-release";
    BuildSpec spec{tree.filename().string() + (parts.size() > 2 ? "/" + fs::path(build).filename().string() : ""),
                   (tree / build / "core" / "bessd").string(),
                   {}};
    std::stringstream fl(flags);
    for (std::string f; std::getline(fl, f, ',');) {
      if (!f.empty()) {
        spec.flags.push_back(f);
        spec.name += " " + f;
      }
    }
    return spec;
  }
  const size_t eq = text.find('=');
  if (eq == std::string::npos) {
    throw std::runtime_error("--build " + text + ": current[:tree[:builddir[:flags]]] or name=<bessd>[,flag...]");
  }
  BuildSpec spec{text.substr(0, eq), "", {}};
  std::stringstream ss(text.substr(eq + 1));
  std::getline(ss, spec.bessd, ',');
  for (std::string f; std::getline(ss, f, ',');) {
    spec.flags.push_back(f);
  }
  return spec;
}

std::optional<double> Median(std::vector<double> v) {
  if (v.empty()) {
    return std::nullopt;
  }
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

std::string Fmt(const char *format, double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), format, v);
  return buf;
}

std::string JsonString(const std::string &s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
      out += escaped;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

std::string JsonNumber(const std::optional<double> &v) {
  return v ? Fmt("%.6g", *v) : "null";
}

int Main(int argc, char **argv) {
  Options o;
  std::vector<BuildSpec> builds;
  std::string tests_arg;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(a + " needs a value");
      }
      return argv[++i];
    };
    if (a == "--tests") {
      tests_arg = value();
    } else if (a == "--build") {
      builds.push_back(ParseBuild(value()));
    } else if (a == "--rounds") {
      o.rounds = std::stoi(value());
    } else if (a == "--cpu") {
      o.cpus.clear();
      std::stringstream ss(value());
      for (std::string c; std::getline(ss, c, ',');) {
        o.cpus.push_back(std::stoi(c));
      }
    } else if (a == "--control-cpu") {
      o.control_cpu = std::stoi(value());
    } else if (a == "--duration") {
      o.duration = std::stod(value());
    } else if (a == "--warmup") {
      o.warmup = std::stod(value());
    } else if (a == "--counters") {
      o.counters = true;
    } else if (a == "--save") {
      o.save = value();
    } else if (a == "--baseline") {
      o.baseline = value();
    } else if (a == "--threshold") {
      o.threshold = std::stod(value());
    } else if (a == "--list") {
      for (const Test &t : Catalogue()) {
        std::printf("%s\n", t.name.c_str());
      }
      return 0;
    } else {
      std::fprintf(stderr,
                   "usage: live_perf [--tests NAME|F|M,...] [--build SPEC]... [--rounds N]\n"
                   "  [--cpu W0[,W1]] [--control-cpu C] [--duration S] [--warmup S] [--counters]\n"
                   "  [--save FILE] [--baseline FILE [--threshold PCT]] [--list]\n"
                   "SPEC: current | current:<tree>[:<builddir>[:<flags,...>]] | name=<bessd>[,flag...]\n");
      return 2;
    }
  }
  if (o.cpus.empty()) {
    throw std::runtime_error("--cpu names no CPU");
  }
  if (builds.empty()) {
    builds.push_back(ParseBuild("current"));
  }
  const std::vector<Test> all = Catalogue();
  std::vector<std::string> wanted;
  std::stringstream ws(tests_arg);
  for (std::string w; std::getline(ws, w, ',');) {
    wanted.push_back(w);
  }
  std::vector<Test> tests;
  for (const Test &t : all) {
    const bool selected =
        wanted.empty() || std::any_of(wanted.begin(), wanted.end(), [&](const std::string &w) {
          return t.name == w || t.name.starts_with(w + ".");
        });
    if (selected) {
      tests.push_back(t);
    }
  }
  const bool any_m =
      std::any_of(tests.begin(), tests.end(), [](const Test &t) { return t.cls == 'M'; });
  if (any_m && std::none_of(tests.begin(), tests.end(),
                            [](const Test &t) { return t.name == "M.base"; })) {
    tests.insert(tests.begin(), *std::find_if(all.begin(), all.end(),
                                              [](const Test &t) { return t.name == "M.base"; }));
  }

  // build -> test -> samples in round order.
  std::map<std::string, std::map<std::string, std::vector<Sample>>> data;
  for (int r = 0; r < o.rounds; r++) {
    std::vector<BuildSpec> order = builds;
    if (r % 2) {
      std::reverse(order.begin(), order.end());
    }
    for (const BuildSpec &spec : order) {
      const auto t0 = Clock::now();
      for (auto &[name, sample] : RunRound(spec, tests, o)) {
        data[spec.name][name].push_back(sample);
      }
      std::fprintf(stderr, "round %d %-10s %.0fs\n", r + 1, spec.name.c_str(),
                   std::chrono::duration<double>(Clock::now() - t0).count());
    }
  }

  // Medians per build, M tests also as the delta above M.base.
  std::map<std::pair<std::string, std::string>, std::optional<double>> med;
  const auto values = [&](const std::string &b, const std::string &t) {
    std::vector<double> v;
    for (const Sample &s : data[b][t]) {
      if (s.ns) {
        v.push_back(*s.ns);
      }
    }
    return v;
  };
  for (const BuildSpec &b : builds) {
    for (const Test &t : tests) {
      med[{b.name, t.name}] = Median(values(b.name, t.name));
    }
  }
  const std::string &first = builds.front().name, &last = builds.back().name;
  std::printf("\n%-20s", "test");
  for (const BuildSpec &b : builds) {
    std::printf(" %12s", b.name.c_str());
  }
  std::printf(" %9s  pairs %s>%s\n", "change", last.c_str(), first.c_str());
  for (const auto &[cls, title] :
       {std::pair{'F', "framework (cross-cutting): ns/packet"},
        std::pair{'M', "modules (localised): ns/packet above M.base"}}) {
    std::printf("-- %s\n", title);
    for (const Test &t : tests) {
      if (t.cls != cls) {
        continue;
      }
      std::vector<std::optional<double>> row;
      for (const BuildSpec &b : builds) {
        std::optional<double> v = med[{b.name, t.name}];
        const std::optional<double> base = med[{b.name, "M.base"}];
        if (cls == 'M' && t.name != "M.base" && v && base) {
          *v -= *base;
        }
        row.push_back(v);
      }
      std::printf("%-20s", t.name.c_str());
      for (const auto &v : row) {
        std::printf(" %12s", v ? Fmt("%.2f", *v).c_str() : "n/a");
      }
      const auto &a = row.front(), &z = row.back();
      std::printf(" %9s", a && z && *a > 0 ? Fmt("%+8.1f%%", (*z / *a - 1) * 100).c_str() : "n/a");
      const std::vector<double> pa = values(first, t.name), pz = values(last, t.name);
      int worse = 0;
      for (size_t i = 0; i < std::min(pa.size(), pz.size()); i++) {
        worse += pz[i] > pa[i];
      }
      std::set<std::string> errors;
      for (const BuildSpec &b : builds) {
        for (const Sample &s : data[b.name][t.name]) {
          if (!s.error.empty()) {
            errors.insert(s.error);
          }
        }
      }
      std::printf("  %d/%zu", worse, std::min(pa.size(), pz.size()));
      for (const std::string &e : errors) {
        std::printf("  %s", e.c_str());
      }
      std::printf("\n");
    }
  }
  if (o.counters) {
    std::printf("\n-- counters per packet on worker 0 (median): instructions / cycles / L1D misses"
                " / branch misses\n");
    for (const Test &t : tests) {
      std::printf("%-20s", t.name.c_str());
      for (const BuildSpec &b : builds) {
        std::string row;
        for (const char *k : {"instructions_per_pkt", "cycles_per_pkt",
                              "L1-dcache-load-misses_per_pkt", "branch-misses_per_pkt"}) {
          std::vector<double> v;
          for (const Sample &s : data[b.name][t.name]) {
            if (s.counters.contains(k)) {
              v.push_back(s.counters.at(k));
            }
          }
          const auto m = Median(v);
          row += (row.empty() ? "" : " / ") + (m ? Fmt("%.1f", *m) : std::string("n/a"));
        }
        std::printf("   %s: %s", b.name.c_str(), row.c_str());
      }
      std::printf("\n");
    }
  }
  if (!o.save.empty()) {
    std::ofstream f(o.save);
    f << "{\n  \"tests\": [";
    for (size_t i = 0; i < tests.size(); i++) {
      f << (i ? ", " : "") << JsonString(tests[i].name);
    }
    f << "],\n  \"builds\": [";
    for (size_t i = 0; i < builds.size(); i++) {
      f << (i ? ", " : "") << JsonString(builds[i].name);
    }
    std::string cpus;
    for (const int c : o.cpus) {
      cpus += (cpus.empty() ? "" : ",") + std::to_string(c);
    }
    f << "],\n  \"cpu\": " << JsonString(cpus) << ",\n  \"rounds\": " << o.rounds
      << ",\n  \"duration\": " << o.duration << ",\n  \"raw\": {";
    for (size_t bi = 0; bi < builds.size(); bi++) {
      f << (bi ? "," : "") << "\n    " << JsonString(builds[bi].name) << ": {";
      for (size_t ti = 0; ti < tests.size(); ti++) {
        f << (ti ? ", " : "") << JsonString(tests[ti].name) << ": [";
        const auto &samples = data[builds[bi].name][tests[ti].name];
        for (size_t si = 0; si < samples.size(); si++) {
          f << (si ? ", " : "")
            << (samples[si].ns ? JsonNumber(samples[si].ns) : JsonString(samples[si].error));
        }
        f << "]";
      }
      f << "}";
    }
    f << "\n  },\n  \"counters\": {";
    // Per build and test, each counter's per-round values (rounds without
    // counters leave none).
    for (size_t bi = 0; bi < builds.size(); bi++) {
      f << (bi ? "," : "") << "\n    " << JsonString(builds[bi].name) << ": {";
      for (size_t ti = 0; ti < tests.size(); ti++) {
        std::map<std::string, std::vector<double>> by_key;
        for (const Sample &s : data[builds[bi].name][tests[ti].name]) {
          for (const auto &[k, v] : s.counters) {
            by_key[k].push_back(v);
          }
        }
        f << (ti ? ", " : "") << JsonString(tests[ti].name) << ": {";
        bool first = true;
        for (const auto &[k, vs] : by_key) {
          f << (first ? "" : ", ") << JsonString(k) << ": [";
          for (size_t i = 0; i < vs.size(); i++) {
            f << (i ? ", " : "") << JsonNumber(vs[i]);
          }
          f << "]";
          first = false;
        }
        f << "}";
      }
      f << "}";
    }
    f << "\n  },\n  \"median\": {";
    for (size_t bi = 0; bi < builds.size(); bi++) {
      f << (bi ? "," : "") << "\n    " << JsonString(builds[bi].name) << ": {";
      for (size_t ti = 0; ti < tests.size(); ti++) {
        f << (ti ? ", " : "") << JsonString(tests[ti].name) << ": "
          << JsonNumber(med[{builds[bi].name, tests[ti].name}]);
      }
      f << "}";
    }
    f << "\n  }\n}\n";
  }
  if (!o.baseline.empty()) {
    std::ifstream in(o.baseline);
    std::stringstream text;
    text << in.rdbuf();
    gp::Struct saved;
    if (auto ok = gp::util::JsonStringToMessage(text.str(), &saved); !ok.ok()) {
      throw std::runtime_error(o.baseline + ": " + std::string(ok.message()));
    }
    const auto &saved_builds = saved.fields().at("builds").list_value().values();
    const auto &ref = saved.fields()
                          .at("median")
                          .struct_value()
                          .fields()
                          .at(saved_builds.rbegin()->string_value())
                          .struct_value()
                          .fields();
    // A test this run has no result for fails: a gate must not pass on an
    // error. One the saved run lacks (a newer test) is only noted.
    std::vector<std::string> failed, uncompared;
    for (const Test &t : tests) {
      const auto it = ref.find(t.name);
      const std::optional<double> z = med[{last, t.name}];
      if (!z) {
        failed.push_back(t.name + " no result");
        continue;
      }
      if (it == ref.end() || !it->second.has_number_value() || it->second.number_value() <= 0) {
        uncompared.push_back(t.name);
        continue;
      }
      const double a = it->second.number_value();
      if ((*z / a - 1) * 100 > o.threshold) {
        failed.push_back(t.name + " " + Fmt("%+.1f%%", (*z / a - 1) * 100));
      }
    }
    const auto join = [](const std::vector<std::string> &v) {
      std::string s;
      for (const std::string &e : v) {
        s += (s.empty() ? "" : ", ") + e;
      }
      return s;
    };
    std::printf("\nagainst %s: %s\n", o.baseline.c_str(),
                failed.empty() ? ("no test slower than " + Fmt("%.1f%%", o.threshold)).c_str()
                               : join(failed).c_str());
    if (!uncompared.empty()) {
      std::printf("not in %s: %s\n", o.baseline.c_str(), join(uncompared).c_str());
    }
    return failed.empty() ? 0 : 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    return Main(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "live_perf: %s\n", e.what());
    return 2;
  }
}
