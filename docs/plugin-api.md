# BESS External Plugin API Specification

This document formalizes the supported C++ and build contract for out-of-tree BESS modules and plugins (such as OMEC UPF). It defines the installed interface boundary, packaging via `bess-dev`, dynamic loading semantics, and ABI expectations.

---

## Stability and ABI

- **Public**: the module, packet and port headers at `bess/core/`,
  `framework/{module_init_context,instance_registry,plugin}.h`, the worker and
  CPU ids they take (`dataplane/{worker_id,cpu_id,strong_id}.h`, D-133), the
  `utils/` helpers in the install, and the generated `pb/*.pb.h` they include. This is
  the supported source API. External plugins must be rebuilt against their
  target BESS release; BESS does not promise a stable C++ ABI.
- **Experimental**: the selected headers under
  `bess/core/{classifier,dataplane,flow,meter,rcu,route,stats}`, the wire
  codec facade (`framework/resource_codec.h`, `framework/resource_bindings.h`,
  D-094), the classifier-table helpers (`framework/table_fields.h`,
  `framework/table_batch.h`, D-189), the table resources and the tables under
  them (`classifier/{exact,masked}_rule_resource.h`,
  `classifier/concurrent_{exact,masked}.h`, `dataplane/rebuild_resource.h`,
  D-191), and the static archives of section 6
  (D-095). Their interfaces may
  change without preserving source compatibility. (`flow/` is
  the flow-state library, see `docs/flow-state.md`; a plugin that uses it
  links the `bess_flow` symbols `bessd` already exports.) A public header
  never includes an experimental one.
- **Internal**: runtime, control-plane, driver, daemon, and test implementation
  headers, `*.grpc.pb.h`, and the control and test protocols. They are not
  installed.

The class of every installed header is one line of
[`tools/api_classes.json`](../tools/api_classes.json); the installed set must
equal that table (`tools/check_installed_headers.py`, run on the staged
install in CI), so a header cannot be installed without being classified or
classified without being installed. Promoting a header is a recorded decision.

One layout choice is bessd's, not the plugin's: whether flow tables check their
owner thread (`flow/owner.h`) changes `WorkerFlowTable`'s layout, so `bess-dev`
carries `-DBESS_FLOW_OWNER_CHECKS=<0|1>` from bessd's build type. A plugin built
with another build type (a release plugin for a debug bessd) still agrees with
the daemon it loads into; a table shared through the `InstanceRegistry` has one
layout on both sides.

The same check compiles a set of twins against the staged install, each a line
that must compile and one that must not, with the compiler's diagnostic
matched (M23): strong ids (`ActionId`, `NextHopId`, `NextHopGroupId`,
`WorkerId`, `MeterId`, `ScopeId`, `InterfaceId`, `BridgeDomainId`,
`RouteDomainId`) never convert to an integer or to each other, nor compare
across kinds; generation handles of different kinds do not convert; a flow
key must be trivially copyable (`FixedFlowKey`), and a padded one needs its
own hash and equality (`FlowKeyOps`); an expiry payload must be trivially
copyable; a `SharedFlowTable`'s mode decides its lookups (a shared-mutable
table has no `FindOwned`, an owned-by-creator table no `Find` or `FindBatch`,
and a table without alias slots no `AddAlias`). A new id type, concept or
mode-restricted member gets a twin in `NEGATIVE_CASES`.

CI then builds `examples/standalone_plugin` from the staged `bess-dev` alone (a
copy of `bess-dev.pc` whose prefix is the stage, so a BESS already installed on
the machine cannot supply a header), loads every plugin into the staged
`bessd`, and runs packets through each, driven through the C++ SDK
(`tests/live/standalone_plugins_test.cc`). One of them, `standalone_flow_count`,
keeps per-flow state in the public `flow::WorkerFlowTable` (a module on a
public library), and `standalone_appliance` is an appliance: its first instance
creates an application-owned object graph (a range classifier yielding an
`ActionId`, an `ObjectTable` from action to gate) in the `InstanceRegistry`,
the second looks it up, and each runs the whole decision in one step. `examples/sdk_battery` is a third-party library on the public
primitives alone (`StrongId`, `GenerationHandle`, `ExpiryWheel`): leases that
expire unless renewed, with handles that go stale when a slot is reused. CI
builds it from the staged install and runs its test without `bessd`.
`examples/appliances` holds the reference appliances (R1-R5, M24) as plugins;
CI loads them into the staged `bessd`, and drives R4's resources over the
control API with its own wire types (below, "Wire codecs").
`examples/static_router` is R1's application linked from the installed
archives with DPDK and no `bessd` (section 6); CI builds and runs it.
`examples/sdk_samples` holds the smallest meaningful program per library --
router, meters, conntrack and NAT, the FDB, handoff and tunnel decapsulation --
each with typed use, an update, error handling as values, and the ownership
rule in its header comment; CI compiles them from the staged install with
warnings as errors.

- `framework/plugin.h` defines the versioned `bess_plugin_descriptor_v1` C
  ABI (below). `ADD_MODULE` remains the module registration mechanism.

## 1. Packaging and Dependency Resolution (`bess-dev`)

BESS installs development headers and a standard pkg-config file:
- **Package name**: `bess-dev`
- **Header root**: `${includedir}/bess` and `${includedir}/bess/core`
- **Package flags**:
  ```sh
  pkg-config --cflags bess-dev
  # Output includes:
  # -I${includedir}/bess -I${includedir}/bess/core
  # -march=<bessd's cpu option> [-DBESS_ARCH_GENERIC]
  # the DPDK include paths, -include rte_config.h and defines bessd compiled with
  # -D_GNU_SOURCE -DGLOG_USE_GLOG_EXPORT -include cinttypes
  # plus Cflags from required dependencies (libglog, protobuf).
  ```

`bess-dev` does not require `grpc++`. Plugins that use gRPC must declare it
as their own dependency.

**A staged prefix.** `bess-dev.pc` names the prefix it was configured with
(`prefix=/usr/local` for a normal install). A `--destdir` install — a stage, or
the CI tree described above — puts the tree somewhere else, so plain
`pkg-config` still resolves `-I/usr/local/include/bess` and a plugin build
fails on `module.h`. Point it at the stage with
`PKG_CONFIG_PATH=<stage>/lib/pkgconfig pkg-config --define-prefix --cflags
bess-dev` (keep `PKG_CONFIG_PATH` absolute, or the derived prefix comes out
relative). Meson honours `PKG_CONFIG`, so the wrapper carries the flag through:
`PKG_CONFIG='pkg-config --define-prefix' PKG_CONFIG_PATH=<stage>/lib/pkgconfig
meson setup build` resolves the stage's includes (verified against a stage — a
C++ plugin built this way compiles `module.h`), and it is how the UPF's meson
builds reach a stage. Passing `-I<stage>/include/bess
-I<stage>/include/bess/core` explicitly also works.

**ISA.** A plugin compiles for bessd's ISA (M21). `bess-dev` does not
`Requires: libdpdk`, because pkg-config would append the `-march`/`-mrtm`
DPDK was built with after bessd's and the last `-march` wins; it carries the
DPDK compile flags bessd used (machine flags removed) and DPDK's shared-library
link flags instead, so the DPDK prefix is the one bessd was built against.
With `-Dcpu=native` "bessd's ISA" means the build machine's. bessd's `-Dtune`
(`-mtune`) is not part of it: tuning changes no instruction a CPU must support,
so `bess-dev` does not carry it and a plugin picks its own (D-127).

**Logging.** Include `"utils/logging.h"`, never `<glog/logging.h>` directly. glog
and protobuf's absl logging both define `LOG`/`CHECK`/`VLOG`; the header included
second wins, and `bessd` initialises only glog. `utils/logging.h` includes absl
first so glog wins in every translation unit (D-059); a plugin that includes glog
itself before any BESS header can end up logging through uninitialised absl.

### Out-of-tree Meson Integration
An external project builds plugins using Meson without vendoring BESS source code:

```meson
project('my_bess_plugin', 'cpp',
  version: '1.0.0',
  default_options: ['cpp_std=c++23', 'buildtype=release'],
)

bess_dep = dependency('bess-dev', required: true)

shared_module('my_module',
  'my_module.cc',
  dependencies: [bess_dep],
  install: true,
  install_dir: '/usr/local/lib/bess/modules',
)
```

---

## 2. Module Lifecycle and Registration

Every external plugin defines one or more classes inheriting from `Module`.

### Registration Macro
The module registers itself into the BESS daemon's runtime registry via the `ADD_MODULE` macro:

```cpp
#include "module.h"

class MyModule final : public Module {
 public:
  static const Commands cmds;
  MyModule() : Module() {}

  CommandResponse Init(const bess::pb::EmptyArg &arg);
  void DeInit() override;
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
};

const Commands MyModule::cmds = {};

CommandResponse MyModule::Init(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

void MyModule::DeInit() {}

void MyModule::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  RunNextModule(ctx, batch);
}

ADD_MODULE(MyModule, "my_module", "Description of external module")
```

When BESS dynamically loads the shared library (`dlopen`), the static constructor generated by `ADD_MODULE` executes and registers the class name in `bess::ModuleBuilder::all_module_builders_holder()`.

### Argument types and schema discovery

`GetSchema` (D-105) tells generic clients (the C++ bessctl, manifests, a UI)
which protobuf type each class's `Init` and each action takes and answers.
`ADD_MODULE` and `ADD_DRIVER` record `Init`'s parameter type themselves. An
action's types are the `arg_type` and `result_type` strings in `cmds`, which
the daemon resolves as a full name (`myplugin.pb.SetArg`), else in the package
of the class's `Init` type, else in `bess.pb`. A plugin whose `Init` takes
`bess::pb::EmptyArg` and whose actions take its own messages must therefore
either give `Init` its own package's type or declare those types by full
name; otherwise `GetSchema` reports `""` and generic clients cannot build the
action's argument.

### Resources and actions (D-129)

A module's state is its init argument plus its **resources**; everything else
a client can do to it is an **action**.

- **Resources** are tables (a map from a key message to a value message) and
  singletons (one value: run-time settings). A class declares them, and the
  daemon refuses to create a module whose registered, codec-bound resources
  named `<module>/<table>` differ from the declaration (kind, key and value
  types, whether workers must be paused):

  ```cpp
  class MyModule final : public Module {
   public:
    static const Commands cmds;
    static const ResourceTables resource_tables;
    ...
  };
  const ResourceTables MyModule::resource_tables = {
      ResourceTable::Map<my::RuleKey, my::RuleValue>("rules"),
      ResourceTable::Singleton<my::Settings>("settings", /*requires_paused_workers=*/true),
  };
  ```

  `GetSchema` lists them; transactions (`ApplyTransaction`) and desired state
  (`Pipeline.resources`, below) change them. A table whose change rebuilds
  its structure and swaps it in - so a change costs in proportion to the
  table, not to the change - says so with `.Rebuilds()`
  (`ResourceTable::Map<my::RuleKey, my::RuleValue>("rules").Rebuilds()`);
  `GetSchema` reports it (`rebuilds`) and `ValidatePipeline` counts it in the
  module's `updates` (D-180).
- **Capabilities (D-180).** A class says who owns the state its packet path
  changes and whether one flow's packets must reach one worker, so a
  controller can place it before it exists (`ValidatePipeline` answers it
  for every module, with how its tables change and their consistency).
  Statistics are not state; configuration the control plane writes is the
  resource tables' business:

  ```cpp
  // Each worker keeps its own flow table; a flow must stay on one worker.
  static constexpr ModuleCapabilities kCapabilities{
      ModuleCapabilities::State::kPerWorker, ModuleCapabilities::FlowAffinity::kRequired};
  ```

  When the init argument decides (ConnTrack's `mode`), declare
  `static ModuleCapabilities Capabilities(const MyArg &arg);` instead. The
  states are `kNone` (the packet path changes nothing), `kSingleWorker` (one
  worker owns it: the class's worker limit is 1), `kPerWorker` and `kShared`
  (several workers own or share it: the limit is above 1); affinity is
  `kNone`, `kPreferred` (correct anywhere, cheaper when a flow stays) or
  `kRequired`, and only per-worker state needs either. A class that declares
  nothing reports both unspecified; every built-in class declares.
  `ModuleCapabilitiesTest` checks declarations for every linked class and
  worker limits for the five resource-module cases and ConnTrack's modes.
  Read `kCapabilities` in a plugin only through its members
  (`kCapabilities.state`). Never copy it, compare it, take its address or bind
  a reference to the whole object: gcc emits an inline variable used that way
  as a `STB_GNU_UNIQUE` symbol, and glibc never unloads a library that defines
  one, so the plugin could not unload. The framework reads only its members.
- **Stating a bound (D-149).** `GetResourceStatus` reports each resource's
  live entries and, when the resource states one, its capacity, so a
  controller sees the ceiling it can plan against before it writes. The
  framework's resources state it for you: `dataplane::SlotResource` (its
  table's id count), `classifier::ExactRuleResource` (what its table accepts,
  when the owner cannot grow it) and a settings singleton (1: the one value
  it holds, which `live` does not count), so a module built on them writes
  nothing. A resource of your own overrides
  `std::optional<uint64_t> Capacity() const` when it knows a firm ceiling on
  entries (`return table_.capacity();`) and leaves it alone otherwise. Two
  rules: not stated is not unbounded (the reply says only that no number was
  given), and never report a growable table's current size as its capacity,
  since the ceiling moves. A ceiling is not a promise of room: a hash table's
  placement can refuse before the table is full, and a refused transaction
  leaves nothing behind.
- **Rule results are configured, not hard-coded (D-172).** A classifier rule's
  value is a gate plus an opaque byte record. The module's argument carries
  `result_fields` (each maps a record slice to a metadata attribute or a packet
  offset) and an optional `default_result` for a miss; `Init` compiles them
  into a `classifier::ResultPlan` and a `classifier::RecordArena`, and the rule
  table stores a record id beside the gate. A field may name a `reference`
  resource: `RAW` uses its bytes as the entry's key, `BE32` treats them as a
  32-bit big-endian id (the metadata convention) and builds the host-order
  key, the rule resource declares the names with the engine, and the engine
  orders a rule after the entries it names and refuses their erase while it
  remains. A referenced resource may declare a key holder that holds some of
  its keys (`Resource::DeclareKeyHolder`, D-187): `<router>/next_hops`
  declares `<router>/neighbors`, which holds the neighbor-bound ids, so one
  column naming `<router>/next_hops` may hold static and bound hops alike, and
  an unbind is refused while a rule names the id. An all-zero field references
  nothing, and the packet path never resolves a reference: it writes bytes the
  transaction already validated. With no result fields the module is gate-only
  and pays nothing for the generality. A metadata field whose attribute no
  downstream module reads (`kMetadataOffsetNoWrite`) is compiled out of the
  plan: it is not written, and the rule's gate and other fields still apply;
  with every field compiled out the module runs gate-only. A field with a
  reader but no offset (out of metadata space, or a reader also reached by a
  path with no writer: `kMetadataOffsetInvalidScope`) still fails the module
  closed to its default gate (D-181).
- **Actions** are the `cmds` entries: verbs that are not state (a lookup, a
  flush, a counter read, a table generator). `InvokeAction` runs them and
  `GetSchema` lists them, with `result_type` naming the message an action puts
  in its answer (empty: it answers nothing). An action holds no state: a
  resource's entries are written through the resource, and an action that can
  write one outside a transaction (L2Forward's `populate`) says so with
  `edits_resources`, which advances the resource generation so a concurrent
  apply conflicts. There is no `LEGACY_ONLY` command and no legacy
  `ModuleCommand` RPC left to run one (D-140).

  ```cpp
  const Commands MyModule::cmds = {
      {"stats", "EmptyArg", MODULE_CMD_FUNC(&MyModule::CommandStats), Command::THREAD_SAFE,
       "MyStatsResponse"},
      {"populate", "MyPopulateArg", MODULE_CMD_FUNC(&MyModule::CommandPopulate),
       Command::THREAD_SAFE, "", /*edits_resources=*/true},
  };
  ```

  `edits_resources` says the action can change a resource's contents (it
  advances the resource generation, so a concurrent `ApplyPipeline` conflicts
  instead of undoing the edit); it defaults to false.

### Plugin descriptor (`framework/plugin.h`)

A plugin may export `extern "C" const BessPluginDescriptor *bess_plugin_descriptor_v1()`.
It is a small C ABI, separate from the C++ module API, that lets `bessd`
diagnose a plugin built for another release before the plugin runs any code
beyond its static constructors. It does not promise that C++ module objects
are ABI-compatible across releases; the API range below says which releases
the plugin's source was written for.

```cpp
struct BessPluginDescriptor {
  uint32_t abi_version;           // BESS_PLUGIN_ABI_VERSION (1): layout of this struct
  const char *name;
  const char *version;
  uint32_t api_min;               // lowest BESS_PLUGIN_API_VERSION the plugin supports
  uint32_t api_max;               // highest BESS_PLUGIN_API_VERSION the plugin supports
  uint64_t required_capabilities; // OR of BESS_CAP_* the plugin needs
};
```

- `BESS_PLUGIN_API_VERSION` is the version of the supported source
  API: it increases when a change to the public headers can break a plugin's
  source. A daemon at version N loads a plugin whose `[api_min, api_max]`
  contains N. It is 4: version 1 became 2 with D-129 (a `ResourceCodec`
  has `kind()` and `Entries()`, a `TypedCodec` takes an `entries` function,
  and a class's codec-bound resources must match its `resource_tables`),
  2 became 3 with D-133 (typed worker ids: `Context::wid` and
  `Worker::wid()` are a `dataplane::WorkerId`, indexed with `.value()`;
  `Worker::core()` is a `dataplane::CpuId` and `Worker::socket()` a
  `dataplane::SocketId`; `EventHub::Post`/`Full`, `EventThrottle::Note` and
  `Module::AddActiveWorker` take a `WorkerId`), and 3 became 4 with D-140
  (a `cmds` entry is a name, an argument type, a handler, its thread safety,
  the answer's type and `edits_resources`: `Command::Exposure`,
  `Command::Effect`, `LEGACY_ONLY` and the rest of that pair are gone, and
  `ModuleBuilder::RunCommand` takes no `CommandSurface`; the gate-hook command
  surface is gone with it -- `GateHookCommand(s)`, `GATE_HOOK_CMD_FUNC`, the
  required static `cmds` member and `GateHook::RunCommand`/`set_builder` -- so
  `ADD_GATE_HOOK` takes no command table).
- `BESS_PLUGIN(name, version)` declares a plugin built against exactly the
  current API (`api_min == api_max == BESS_PLUGIN_API_VERSION`) that needs no
  optional capability. `BESS_PLUGIN_REQUIRES(name, version, capabilities)` is
  the same with the capabilities named. A plugin that must span releases
  defines the function itself with a wider range (see
  `sample_plugin/modules/incompatible_probe.cc`, which does the reverse: it
  declares an API range no daemon supports, to test refusal).
- Capabilities are optional daemon facilities; a daemon refuses a plugin that
  requires one it was not built with:

  | Flag | Facility |
  |---|---|
  | `BESS_CAP_INIT_CONTEXT` | `Module::init_context()` |
  | `BESS_CAP_INSTANCES` | `init_context().instances()` |
  | `BESS_CAP_RESOURCES` | transactional resources (`init_context().resources()`) |
  | `BESS_CAP_METRICS` | operational metrics (`init_context().metrics()`) |
  | `BESS_CAP_REQUESTS` | worker-to-control requests (`init_context().requests()`) |
  | `BESS_CAP_EVENTS` | operational events (`init_context().events()`) |
  | `BESS_CAP_EVENT_SOURCES` | named event sources and the throttled or threshold packet-path events (`EventHub::NamedSource`, `EventHub::Full`, `stats::EventThrottle`, `stats::CrossingNotifier`) |
  | `BESS_CAP_TICKS` | periodic control-side work (`init_context().ticks()`) |

  The `standalone_*` conformance plugins in `examples/standalone_plugin` use
  `BESS_PLUGIN`; `sample_plugin`'s `SequentialUpdate` uses
  `BESS_PLUGIN_REQUIRES(..., BESS_CAP_INIT_CONTEXT)`.

When `bessd` loads a plugin (`--modules` directory or `bessctl plugin import`) it
`dlopen`s it, looks up `bess_plugin_descriptor_v1`, and checks in this order:
`abi_version` equals the daemon's, `api_min <= api_max`, the daemon's API
version lies in the range, and every required capability is provided. A
plugin without a descriptor is a legacy plugin and loads unchecked. The
descriptor's name and version are logged on success.

**A refused plugin** is logged (`Plugin <path> refused: <reason>`, naming the
daemon and plugin versions or the missing capability bits) and then
`dlclose`d; it never appears in `ListPlugins`. Static constructors have
already run by then, since `dlopen` runs them before the descriptor can be
read. For modules registered with `ADD_MODULE` that is harmless: `dlclose`
runs the matching static destructor, which deregisters the class (the sample
test checks that `IncompatibleProbe` is absent after refusal). Any other
side effect of a plugin's static initializers is the plugin's own to avoid:
keep them free of effects, and do real setup in `Init()`. At daemon start a
refusal is a warning and the daemon keeps running with the remaining plugins;
`bessctl plugin import` returns a generic `Failed loading plugin` error and the
reason is in the daemon log only.

### Runtime capabilities (`init_context()`)

A module reaches runtime facilities through the protected
`Module::init_context()` (`framework/module_init_context.h`), during
construction or `Init()`:

| Method | Capability |
|---|---|
| `resources()` | register and release transactional resources, read reference counts (`dataplane::ResourceRegistry`, `dataplane/resource_registry.h`, experimental); applying transactions is the control plane's |
| `rcu()` | the reader domain published tables retire through |
| `ports().Find(name)` | look up an existing port by name |
| `instances()` | borrow application-owned shared state (below) |
| `metrics()` | register a source that reports this module's counters when the control plane asks (`stats::MetricRegistry`, `stats/metric_registry.h`, experimental); keep the returned `MetricSource` as a member declared after what it reads |
| `requests()` | open a `RequestEndpoint` (`framework/module_requests.h`, experimental): workers post a small request (a table nearly full), the daemon's maintenance loop calls the module's handler under the control-plane lock; one pending request per endpoint, re-armed by `Done()` |
| `ticks()` | the daemon's `TickHub` (`framework/module_tick.h`, experimental, D-150). Open a `TickEndpoint` on it: the maintenance loop calls the module's callback under the control-plane lock, no sooner than its `period` after the previous start, with a time slice the callback checks as `ctx.Expired()` and resumes next tick; `ctx.Emit(type, source, fields)` queues an event the loop appends to the log after the lock is released. `interval()` is how much time one tick stands for (zero when the loop is disabled, `--maintenance_interval_us=0`, and then no endpoint is ever called), so `Init` can turn a configured duration into ticks; the daemon starts the loop before it serves RPCs, so `Init` never reads zero while a loop runs. `current_tick()` is the tick count of the latest run, the clock a module started from `Init` should start at. `set_interval()` is for the loop and tests standing in for it; a module never calls it. For work that belongs on a clock on the control side (reading the module's own counters against limits a controller armed, ageing, fill checks): the framework knows nothing about what the callback does, and a controller reaches the module through its own action. For counters against armed limits, `framework::ThresholdTable` (`framework/threshold_table.h`, experimental, D-151) is the table the callback sweeps: level-triggered, one report per arming, bounded by the slice, reported as one `bess.counter_threshold` event (source the module, fields `key`, `value`, `limit`) through `ReportCrossings` |
| `events()` | register an event type in Init (`RegisterType(name, value names)`); a worker posts with `Post(ctx->wid, WorkerEvent{...})`, never blocking (a full ring refuses and counts); the maintenance loop moves events into the log `WatchEvents` streams (`stats::EventHub`, `stats/event_hub.h`, experimental). `NamedSource(name)` makes the log show a source by name. For a recurring packet-path condition (a full table or queue), `stats::EventThrottle` (`stats/event_throttle.h`) posts at most one event per second per worker carrying the count, called on the failure path only |
| `codecs()` | bind a wire codec to each resource registered through `resources()`, so `ApplyTransaction` and `ListTransactionResources` reach it (`framework::ResourceBindings`, experimental; below) |

Every type this surface names is installed.

**Counter crossings.** A controller that wants to know when a counter passes a limit is told by
one event shape, `bess.counter_threshold` (source: the module; fields `key`, `value`, `limit`),
from whichever of two producers fits the workload. The module's own tick sweeps a
`framework::ThresholdTable` (D-151), which reads every worker's plane, so it is exact for any key
and costs the packet path nothing; the latency is the maintenance interval. A worker can instead
post the crossing itself with `stats::CrossingNotifier` (`stats/crossing_notifier.h`,
experimental, D-152): one compare, `Crossed(before, after, limit)`, after the increment it
already makes, and a `Note(...)` only on a crossing. That is exact only when a key's traffic is
pinned to one worker, because a worker sees its own plane alone; for a key spread across workers
its event is a hint (`WorkerLimit(total, workers)` is the per-worker limit that cannot miss), and
the controller reads the total before it acts. A worker event can be lost when its ring is full
(counted, shown as `bess.events_lost`), so a consumer that cannot afford a missed report keeps
polling as the backstop. The worker path's event also carries `worker`, the plane that crossed.
The SDKs' `WatchCounters` (Go) and `watch_counters` (Python) narrow the stream to one module's
crossings and parse their fields (`docs/control-sdk.md`), so a controller need not know this
shape; a gap in the log or a restart ends that watch with an error rather than passing it over.

**Shared observation.** `ListMetrics`, `bessctl metrics` and HTTP `/metrics`
include these framework observations without a module registering a source:

| Metric | Kind | Label | Meaning |
|---|---|---|---|
| `bess_module_drops_total` | counter | `module` | Packets recorded by the module's drop and forwarding helpers, including unconnected outputs |
| `bess_resource_entries` | gauge | `resource` | The reachable resource's `LiveCount()` |
| `bess_resource_capacity` | gauge | `resource` | Its stated `Capacity()`; omitted, not zero, when no capacity is stated |

The drop total lasts for the module instance's lifetime. Placement bookkeeping
does not reset it. The base uses `KeyedCounters` with one aggregate key and cell,
and a plane for every possible worker slot: worker IDs can be sparse and old
workers' contributions must survive later placement changes. Reads are safe
while workers count; the total is a sum of per-worker observations, not one
global instant. Module-specific loss reports remain separate; this does not
assign a common drop-reason taxonomy.

Resource metrics and `GetResourceStatus` use the same codec-bound reachability.
They report configured resources, not an inferred ceiling for every runtime
flow table.

**Fill levels.** A resource is watched by the daemon itself, with no module involvement: on its maintenance
tick it reads every reachable resource's `LiveCount()` against its level -- the count the resource states in
`FillLevel()`, or the first count that reaches 80% of its `Capacity()` when `FillLevel()` is unstated (D-165,
D-166, `control/resource_fill.h`) -- and reports one `bess.resource_fill` event per crossing: fields
`resource`,
`count` and `limit`, and `capacity` when the resource states one, source the resource's module. It reports
once, and again only after the fill has fallen below the level (or after the resource has stopped being
reachable and come back, which is a new table). A resource that states neither a level nor a capacity (a
tuple space that grows with the workload) is not watched, and a table that fills and drains inside one tick is
not seen: this samples. The sweep runs with the control-plane lock held, like a module's tick, so a table
filling up is reported without a controller polling `GetResourceStatus`.
The sweep resumes after its last examined name when its slice expires, rather
than restarting at the first resource. Unbound names also consume the slice.
Names inserted before the cursor are visited in the next pass. History pruning
also resumes within the slice; surviving resources retain their crossing history.
Stop checks are cooperative, not a hard latency bound: preparing the sorted
registry snapshot is still control-plane work (D-193).

**Where a module's counters live.** A module's counters are per key and per worker, and the shared
structure for them is `stats::KeyedCounters` (`stats/keyed_counters.h`, public, D-154/D-193):
`keys` x `cells_per_key` 64-bit cells in one plane per worker, sized to the worker count the
module passes, so a module on 8 workers pays 8 planes and not the 64 worker slots the daemon
reserves. `CounterSet` is the other shape: a small fixed schema of cells, not a key space. A
worker adds with `ForWorker(wid).Add(key, cell, n)` -- a relaxed load, an add and a relaxed store
to a line it owns, no lock and no allocation -- and the control side reads `Read(wid, key, cell)`
or `Total(key, cell)`, the sum over the planes, which is what a `ThresholdTable` read returns.
The keys are the module's own dense indices, so nothing hashes or grows on the packet path. A
reused key keeps its value: a module that reuses a slot either subtracts the value it recorded
when it re-armed the key, or keeps its keys monotonic. `keys`, `cells_per_key` and `workers` are
all at least 1, and that is a promise rather than a check: a module that owns a capacity refuses
a zero one in its own `Init`.

The context is bound when the `Module` base is constructed, so it is usable in
a derived constructor's member initializers (for example
`published_(init_context().rcu())`). There is no other way to obtain a
context: `ModuleInitContext::ProcessDefault()` is private to `Module`'s
constructor, so a module cannot build or look one up for itself. Never call
`init_context()` from `ProcessBatch` or `RunTask`. Modules in `core/modules` must not include `runtime/`; `tools/check_includes.py`
enforces that.

### Wire codecs (`init_context().codecs()`, experimental, D-094)

A dataplane resource knows nothing about encoding (D-044). To make one
reachable over the control API, the module that registers it binds a codec:
the protobuf key and value messages of the resource, two functions that
convert them to the resource's key bytes and value (`std::any` of the
resource's value type), and a third that reads the committed entries back as
messages. `framework::TypedCodec<KeyMsg, ValueMsg>`
(`framework/resource_codec.h`) builds one from the three functions; a decode
error is an `std::unexpected` string, reported as the operation's error.
`Bind()` returns a `ResourceBinding`: keep it as a member declared after the
resource and reset it before the resource is released.

The read-back (`GetPipeline` with resources, D-129) visits every committed
entry, under the control-plane lock, from the module's control-side view.
It must give each entry in **canonical form**: the message a client upserts
reads back byte-identical under deterministic serialization (defaults
omitted, one spelling per value: lower-case MAC addresses, the network
address of a prefix). Otherwise desired state that came from a read applies
back as an update, every time.

An optional fourth argument, a predicate that is true while a worker runs
the module (`[this] { return HasRunningWorker(); }`), marks a resource whose
values workers read unsynchronized: `ApplyTransaction` refuses it
(`RESOURCE_BUSY`) while a worker runs, and `ApplyPipeline` pauses the
workers for it.

**A value may be empty on purpose, and then the read-back is not a source of
truth.** A value is copied on every publish, so state that changes per packet
belongs in the module's per-worker planes: a counter kept in a value would be
zeroed by a handover, because a publish copies what the control side last wrote.
The read-back (`Contents`, `GetPipeline`) returns such an entry's identity and
size and nothing that changes - a tick built on it would read a row per key and
decide every report from zeros. That state is reached through the module's own
command instead: the UPF's counter module reads its per-worker planes from
`RuleCounts(ctx, module)` and names the module, and a `Contents` read of the same
resource returns 90 B per rule carrying no counts. The line to keep: a resource
is the desired state a client programs and a worker reads; a worker's accumulated
state is the module's own.

**An index a module uses in its own keys is the graph's, not the module's.** A
module that keys by a "domain" (a routing instance, a slice, a tenant) does not
invent the number: it is the slot the graph assigned, the same discipline as a
gate index, which belongs to the connection rather than to the module. Read it
from the deployment's own configuration at Init and refuse a value the graph does
not carry, so two instances that agree on the graph agree on the keys - and a
downlink rule written against one deployment's numbering cannot silently land in
another's table.

```cpp
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "my_wire.pb.h"  // the plugin's own messages, compiled with protoc

// Init(), after init_context().resources().Register(&rules_):
using Codec = bess::framework::TypedCodec<my::RuleKey, my::RuleValue>;
binding_ = init_context().codecs().Bind(
    rules_, std::make_shared<Codec>(
                [](const my::RuleKey &k) -> std::expected<bess::dataplane::ResourceKey, std::string> {
                  if (k.id() == 0) return std::unexpected("rule id 0 is invalid");
                  return bess::dataplane::EncodeKey(RuleId(k.id()));
                },
                [](const my::RuleValue &v) -> std::expected<std::any, std::string> {
                  return std::any(Rule{v.ue(), ActionId(v.action_id())});
                },
                [this](const Codec::TypedVisitor &visit) {
                  for (const auto &[id, rule] : committed_) {
                    my::RuleKey k;
                    k.set_id(id);
                    my::RuleValue v;
                    v.set_ue(rule.ue);
                    v.set_action_id(rule.action);
                    visit(k, v);
                  }
                }));
```

**Singletons** (`framework/singleton_resource.h`, experimental): a
`framework::Singleton<ValueMsg, Staged>` member registers the resource and its
codec together. `stage` validates a value and builds everything it needs
(it may allocate; its error refuses the transaction); `apply` publishes the
staged value and must neither fail nor allocate; `read` returns the current
value in canonical form. A singleton has no key (or `google.protobuf.Empty`)
and cannot be erased. With no worker predicate `apply` runs while workers
run, so what it writes must be safe for them (relaxed atomics, or an
RCU-published object: the `Register` overload whose `apply` also takes the
`dataplane::Retirer` retires the replaced object, within the
`Resource::Footprint` it declares). `Release()` (also on destruction) undoes
the registration; call it in `DeInit`.

```cpp
// Init():
auto registered = settings_.Register(
    init_context().resources(), init_context().codecs(), name() + "/settings",
    [](const my::Settings &s) -> std::expected<uint32_t, std::string> {
      if (s.burst() > 32) return std::unexpected("burst must be [0,32]");
      return s.burst();
    },
    [this](uint32_t &burst) { burst_.store(burst, std::memory_order_relaxed); },
    [this] {
      my::Settings s;
      s.set_burst(burst_.load(std::memory_order_relaxed));
      return s;
    },
    nullptr);
```

A plugin with its own messages links the generated code and `protobuf`
(`bess-dev` already requires it, for the same runtime bessd uses); its
`.proto` file name and package must not collide with BESS's or another
plugin's (the generated code registers them in the process's descriptor
pool). A controller needs the message types: `examples/appliances/session`
installs a descriptor set (`protoc --descriptor_set_out`) next to the plugin,
which a client loads with `google.protobuf.message_factory`. Codecs and
protobuf stay out of the dataplane libraries (`tools/check_includes.py`).

A plugin that binds codecs requires `BESS_CAP_RESOURCES` (its resources) and no
capability of its own: `codecs()` is an inline accessor of an existing context
member, and `ResourceBindings::Bind` and `ResourceBinding::Reset` have been in
every `bessd` that checks a plugin descriptor (D-094).

### Application instances (`init_context().instances()`)

State shared by several module instances lives in an explicit, named
instance rather than a global service (`framework/instance_registry.h`):

```cpp
// Application code (control plane), once:
auto created = runtime.instances().Create<UpfState>("upf0", options);

// In each module's Init(), once:
auto lease = init_context().instances().Lookup<UpfState>("upf0");
if (!lease) {
  return CommandFailure(ENOENT, "%s", InstanceErrorName(lease.error()));
}
lease_ = std::move(*lease);   // keeps the instance alive
state_ = lease_.get();        // cached; used from ProcessBatch
```

`Lookup` never creates, a wrong type is an error, and `Destroy` is refused
while any module holds a lease. The packet path uses the cached pointer only:
no name lookup, no reference count. Create and destroy are structural
operations done under the control-plane lock; the state's own contents must
provide their own synchronization for live updates.

An instance a plugin creates holds the plugin's code (its deleter and type
information) for as long as the instance exists, and nothing destroys
instances on `reset_all`. A plugin that creates registry instances
(`standalone_appliance` is one) cannot be unloaded while they exist: destroy
them first (`Destroy`, once no lease is held), or keep the plugin loaded for
the daemon's life. `unload plugin` refuses (EBUSY, naming them) while any
module of the plugin's classes, port of its drivers, gate hook of its classes
or instance its code destroys exists; it then destroys what the plugin's code
retired to RCU before `dlclose` (D-090). Pointers to the plugin's code that
another plugin or module keeps on its own are not tracked.

### Classifier tables in a module (experimental, D-189)

`WildcardMatch` and `ExactMatch` are built from installed helpers, and a plugin
that holds one or several classifier tables uses the same ones. The pure half is
`classifier/table_schema.h` (no Module, gate or protobuf; compiled into
`bess_classifier`, which `bessd` exports):

| Piece | What it does |
|---|---|
| `KeyLayout`, `CompileKey`, `CompiledKey` | a dense key `field0 \|\| field1 \|\| ...` from resolved source offsets, compiled into an `ExtractPlan` (bounds checked), with the offsets it baked; `CompileKey` refuses a layout with bytes no field writes |
| `ResultFieldSpec`, `ResultSchema` | the slices of an entry's record and where they go: `Check` a field, `References` decodes the entries a record names (D-172), `Compile` builds the `ResultPlan` for a set of destinations, leaving out `kSkippedOffset` (D-181) and refusing `kUnresolvedOffset` |
| `CompiledResult` | the plan, its baked destinations, whether it waits for metadata offsets, the `RecordArena` |

The module-bound half is header-only (`bess_framework` does not link the
classifier, so its calls are emitted in the module that uses it):

| Header | Piece | What it does |
|---|---|---|
| `framework/table_fields.h` | `KeyFields` | the key fields (packet offset or metadata attribute) and the module's limits; resolves them against the module's attribute offsets into a `CompiledKey`, optionally with a per-field mask; a fail-closed key; `Changed` at PreResume |
| | `ResultFields` | parses `result_fields` and `default_result` (registering write attributes), owns the `RecordArena`, checks a rule value (`CheckRule`), compiles the placement (deferred until the first PreResume when it writes metadata), `Compare`s destinations at PreResume |
| | `PublishGeneration`, `RepublishGeneration`, `RebuildGeneration` | publish a module's own generation struct through its `RcuPtr` and reclaim; the PreResume rebuild that falls back to a fail-closed generation |
| | `ReleaseTableResource` | the DeInit release of a rules resource (for teardown when its values reference other resources) |
| `framework/table_batch.h` | `ExtractKeys` | the batch's keys through a `CompiledKey`'s plan, each extracted row written whole, failed rows zeroed |
| | `PlaceResults` | the records a batch matched written through a `ResultPlan`; a packet that cannot be made writable goes to `DROP_GATE` |

The generation struct is the module's: it holds a `CompiledKey` per table key, a
`CompiledResult` per record layout, its tables and whatever else its packet path
reads (a default gate, a specialised kernel). The packet-path helpers are inline
and take the caller's stack scratch: no allocation and no indirect call per
batch. The tables and their rule resources are installed too (next section).

### Table resources: in place or rebuilt (experimental, D-191)

A table a controller writes is a `dataplane::Resource` (`<module>/<table>`).
Two kinds come ready-made; pick by whether the table can change one entry at a
time while workers read it:

| | In place | Rebuilt |
|---|---|---|
| Adapter | `classifier::ExactRuleResource` (`exact_rule_resource.h`), `classifier::MaskedRuleResource` (`masked_rule_resource.h`) | `dataplane::RebuildResource<Id, Rule, Table, Build>` (`rebuild_resource.h`) |
| Table | `ConcurrentExactTable` (rte_hash, lock-free readers), `ConcurrentMaskedTable` (tuple space over it, with ranges) | yours: any immutable structure `Build` makes from the rules, read through your `RcuPtr<Table>` |
| Key, value | the key bytes (masked: mask, value, ranges) to a gate and a result record (`RuleResult`, `MaskedRuleResource::Value`, records in a `RecordArena`) | `EncodeKey(Id)` to your `Rule` |
| Cost of a transaction | per operation, flat in table size: `Reserve()` places a pending entry, `Publish()` stores it | one `Build` of the whole table, however many operations it carries |
| References | values may name other resources (`references`, `may_reference`) | none |
| Readback | the table's own `ForEach` | `rules()`: the committed rules by id |

Use the in-place kind when the table is an exact or ternary match on key bytes
and updates must stay cheap at any size. Use the rebuilt kind for a structure
that cannot change in place: a sorted list, a compiled trie, a
first-match-wins policy whose order is derived from all its rules. Declare such
a table `.Rebuilds()` in `resource_tables`, so `GetSchema` and
`ValidatePipeline` report what an update costs. A rebuilt resource
(`Firewall`'s `<name>/rules` is one):

```cpp
// Every rule, in id order ({id, rule} entries, size()), to the table they
// make, or why they make none.
static std::expected<std::unique_ptr<const MyTable>, std::string> BuildTable(
    const bess::dataplane::RebuildRules<uint32_t, MyRule> &rules);
using Rules = bess::dataplane::RebuildResource<uint32_t, MyRule, MyTable,
                                               decltype(&BuildTable)>;

bess::rcu::RcuPtr<MyTable> table_{init_context().rcu()};  // the packet path reads it
auto rules = std::make_unique<Rules>(name() + "/rules", table_, &BuildTable, "a rule");
rules->Initialize({});  // the first table, before Register()
```

`Reserve()` records each operation (an upsert's map node is allocated there),
`Prepare()` builds once from the committed rules and the operations merged in
id order (a view, not a copy), and the transaction's first `Publish()`
exchanges the pointer, retires the old table and moves the nodes without
allocating; a refused build leaves the published table untouched.
`examples/standalone_plugin/standalone_rule_tables.cc` holds one table of each
kind from the installed headers.

---

## 3. Data Structures and Hot Paths

### `bess::PacketBatch` (`pktbatch.h`)
The unit of packet transport between modules:
- Fixed-capacity array of packet handles: `kMaxBurst = 32`.
- `batch->cnt()`: number of active packets in the batch.
- `batch->packet(i)`: access non-owning `bess::PacketRef` for packet inspection and header manipulation.
- `batch->handles()`: direct pointer to array of DPDK `rte_mbuf*` pointers.

### Packet Forwarding Interfaces (`module.h`)
- **Single-gate batch pass-through (Preferred)**:
  - `RunNextModule(ctx, batch)`: passes the entire batch to output gate 0.
  - `RunChooseModule(ctx, gate_idx, batch)`: passes the entire batch to `gate_idx`.
  - Zero redundant gate lookups and zero packet copying.
- **Per-packet gate decisions (classifiers)**:
  - `EmitBatch(ctx, batch, gates)`: sends packet `i` to `gates[i]`. When every packet has the same gate, the whole batch goes through `RunChooseModule`; otherwise it emits packet by packet. The batch is consumed. Prefer it to an `EmitPacket` loop whenever bursts are often uniform (D-099).
  - `EmitPacket(ctx, pkt, gate_idx)`: routes an individual packet to `gate_idx`, for paths that decide and emit in one pass.
  - `DropPacket(ctx, pkt)`: places packet in the task's dead batch for batched memory reclamation.

---

## 4. Metadata Chaining

Cross-module state in a pipeline (e.g. classification result $\to$ meter $\to$ routing decision) travels in metadata attributes:
- A write happens only where something reads: offsets are computed from writer to reader, so an attribute nothing reads gets **no offset at all** and the write is skipped. A module that publishes a result must therefore tolerate a graph that does not consume it - and a graph that expects the result must contain a reader, or the producer silently writes nothing.
- Attributes are declared during `Init`:
  ```cpp
  meter_id_attr_ = AddMetadataAttr("meter_id", sizeof(bess::utils::be32_t),
                                   bess::metadata::Attribute::AccessMode::kRead);
  ```
- When the name is a policy identity (what a meter reads, what a classifier rule's result writes), make it configuration and keep the historical name as the default (D-168). The name is the whole contract between a writer and a reader: two `Meter`s over one classifier result read `flow_meter_id` and `session_meter_id`, and neither module knows the other exists.
  ```cpp
  const std::string id_attr = arg.id_attr().empty() ? "meter_id" : arg.id_attr();
  meter_id_attr_ = AddMetadataAttr(id_attr, sizeof(bess::utils::be32_t),
                                   bess::metadata::Attribute::AccessMode::kRead);
  ```
- Physical offsets are resolved at pipeline resume (`PreResume`):
  ```cpp
  int MyModule::OnEvent(bess::Event event) {
    if (event == bess::Event::PreResume) {
      action_offset_ = attr_offset(action_id_attr_);
    }
    return 0;
  }
  ```
- Packet metadata is read and written using type-safe offset accessors:
  ```cpp
  _set_attr_with_offset<bess::utils::be32_t>(action_offset_, pkt, bess::utils::be32_t(action_id));
  ```
- A module declares a name once. The one exception is a module that reads an
  attribute and writes it too (a classifier that keys on `route_domain` and sets
  it in its result): a `kRead` and a `kWrite` of the same name and size become
  one `kUpdate` attribute with one index, keeping the reader's contract and
  default. Any other repeat, or a size mismatch, is `-EEXIST`.

### Metadata contracts

The access mode says what a module does with an attribute (`kRead`, `kWrite`,
`kUpdate`). A contract says what a **reader** may assume when the graph does not
write it; the two are orthogonal, and a writer's contract is ignored.
The third argument of `AddMetadataAttr` selects the access mode; the fourth
selects its contract. Reads and updates are required by default (D-195).
Choose best-effort or a declared default explicitly for an optional input:

| Contract | Declaration | A reader with no provider |
|---|---|---|
| required (default) | `AddMetadataAttr(name, size, kRead)` | Resume fails with `EINVAL` naming the module and attribute. `ApplyPipeline` that resumes workers reports the same refusal; the workers stay paused. |
| best-effort | `AddMetadataAttr(name, size, kRead, Attribute::Contract::kBestEffort)` | `attr_offset()` is `kMetadataOffsetNoRead`; the module uses its intentional fallback or diagnostic behavior. |
| optional with default | `AddMetadataAttrWithDefault(name, size, kRead, bytes)` (or `kDefault` for a zero default) | `attr_offset()` is `kMetadataOffsetDefault`; `ResolveAttr<T>(id).Get(pkt)` returns the declared value and never touches packet metadata. |

A required reader also needs a usable packet offset. If its live range does
not fit in packet metadata, resume fails with `ENOSPC`, not a missing-writer
error. A defaulted reader with a provider fails the same way: it cannot hide an
unallocatable provider by reading the default instead. Best-effort allocation
failures remain diagnostic.

Existing three-argument readers now reject incomplete graphs instead of
starting workers that only log or drop. The intentional optional in-tree reads
are explicit: Measure can use a checked in-packet timestamp; PacketStore uses
the first bound key or flow 0; MTTest observes allocation without reading packet
values. Router's route-domain and neighbor-interface constants remain declared
defaults. Registration failures must be returned from `Init`, even for an
optional read; an error code is never an attribute id.

A defaulted attribute needs no provider-side change: if a writer appears in a
later graph revision, the next resume assigns a real offset and the same
`ResolvedAttr` reads the packet. Resolution happens once per batch, so a
default costs no per-packet work and the valid-offset path is the load it was:

```cpp
// In Init:
static constexpr std::array<uint8_t, 4> kDefaultDomain{};
domain_attr_ = AddMetadataAttrWithDefault(
    "route_domain", sizeof(bess::utils::be32_t),
    bess::metadata::Attribute::AccessMode::kRead, kDefaultDomain);

// In PreResume (or lazily on the first run):
domain_ = ResolveAttr<bess::utils::be32_t>(domain_attr_);

// In ProcessBatch:
auto domain = domain_.Get(pkt);
```

`bess::utils::be32_t` is used above because metadata bytes are copied as the
writer stored them; `ResolvedAttr<T>::default_value` is assembled from the
declared bytes with `memcpy`, the same way `get_attr_with_offset<T>` reads a
packet.

**Validation report.** Every resume/apply with workers paused computes the
graph's metadata categories. `GetPipeline` returns it with
`include_metadata: true` as `MetadataReport`, and `bessctl metadata` prints it:
writers without a reader, readers without a writer, unsatisfied required
declarations, applied defaults, `allocation_failures`, and declarations of one
name that disagree about contract or default bytes or have multiple writers.
Multiple writers are not automatically invalid: alternative upstream producers
can share a valid binding. A declaration's `size` conflict is refused at
registration, so it cannot become a usable reader.

This validates declared graph coverage and storage, not packet contents.
It does not prove that a conditional producer wrote a value for every packet,
nor a packet's parse, length or exclusive ownership.

---

## 5. Dynamic Loading Semantics

1. **Daemon launch flag**:
   `bessd --modules=/usr/local/lib/bess/modules`
   Loads all `.so` modules found in the directory before starting listening for RPCs.
2. **Runtime CLI command**:
   `bessctl plugin import /path/to/my_module.so`
3. **Symbol resolution**: `bessd` is linked with `-rdynamic` (`export_dynamic: true` in Meson). Symbols defined in `libbess_framework`, `libbess_execution`, `libbess_eal`, and `libdpdk` are exported, allowing loaded modules to resolve runtime methods without duplicating engine code.
   A plugin never links the static archives of section 6: `bessd` already
   provides those symbols, and a second copy in the plugin would have state of
   its own (the EAL's, the RCU domain's).

---

## 6. Static archives for programs without `bessd` (experimental, D-095)

The libraries that implement the router and its FIB (`bess_route`), the
shared flow index and its classifier backend (`bess_flow`, `bess_classifier`),
the resource and transaction core (`bess_dataplane_core`), the RCU domain
(`bess_rcu`), and the EAL bring-up and utilities they link against
(`bess_eal`, `bess_utils`) are installed as static archives in
`${libdir}/bess`, with `bess-dev-static.pc`. A plugin's unit test may link
them to build the `flow::SharedExactIndex` its resource wraps; the plugin
itself never does:

```sh
pkg-config --libs bess-dev-static
# -L${libdir}/bess -lbess_route -lbess_flow -lbess_classifier -lbess_dataplane_core -lbess_eal -lbess_rcu -lbess_utils
# then bess-dev's flags (DPDK's shared libraries, glog, protobuf) and gflags
```

A program (no `bessd`, no `Module`, no framework) owns an
`rcu::RcuDomain` and creates a `route::Router` on it; the first table brings
the EAL up without hugepages (512 MB of normal pages; `BESS_DPDK_NOHUGE_MB`
sizes it). `examples/static_router` runs R1's `RouterApp`
(`examples/appliances/router/router_app.h`) this way and checks a forwarded
frame; CI's install check builds it from the stage and runs it.

No ABI promise: the archives are built with bessd's compiler, options and ISA
and are linked only by a program built against the same install. They are
compiled as executable (PIE) code, not `-fPIC` (D-119): a shared library or
plugin cannot link them. The transaction engine's header stays internal (applying transactions is the
control plane's, D-074): `bess_dataplane_core` is installed because the router
needs it (its resources), not as an engine a program can drive. Other
libraries (classifier, flow, meter, stats) are not installed as archives yet.
