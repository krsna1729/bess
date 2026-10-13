// SPDX-License-Identifier: BSD-3-Clause

#include "manifest.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/type_resolver_util.h>

#include <format>
#include <regex>
#include <utility>

#include "json.h"
#include "yaml_io.h"

namespace bessctl {

namespace {

namespace gp = google::protobuf;

constexpr std::string_view kUrlPrefix = "type.googleapis.com";

std::string TypeUrl(std::string_view type) {
  return std::format("{}/{}", kUrlPrefix, type);
}

template <typename Status>
std::string Message(const Status &status) {
  return std::string(status.message().data(), status.message().size());
}

// protobuf's parse error for the text bessctl rewrote: its "near L:C (offset
// N)" points into that text, not the user's file, so it goes; the field path
// ("modules[0].arg...") stays. Runs of spaces become one.
std::string ParseError(std::string text) {
  static const std::regex position(R"(,?\s*near\s+\d+:\d+\s+\(offset\s+\d+\):?)");
  static const std::regex spaces(R"(\s{2,})");
  text = std::regex_replace(text, position, ":");
  return std::regex_replace(text, spaces, " ");
}

// Adds `file` (from this program's generated pool) and its imports, unless
// `pool` already has them by name.
bool AddGenerated(const gp::FileDescriptor *file, gp::DescriptorPool &pool) {
  if (pool.FindFileByName(std::string(file->name())) != nullptr) {
    return true;
  }
  for (int i = 0; i < file->dependency_count(); i++) {
    if (!AddGenerated(file->dependency(i), pool)) {
      return false;
    }
  }
  gp::FileDescriptorProto proto;
  file->CopyTo(&proto);
  file->CopyJsonNameTo(&proto);
  return pool.BuildFile(proto) != nullptr;
}

using TypeMap = std::map<std::string, std::string, std::less<>>;

// Appends `key`: `value` to the object `o`. Appended, not inserted: inserting
// moved every member through a temporary that GCC 14 at -O3 flagged as
// maybe-uninitialized (a false positive); the value is assigned after for
// the same reason.
void Add(json::Object &o, std::string key, json::Value value) {
  o.push_back({std::move(key), json::Value{}});
  o.back().value = std::move(value);
}

// Gives the object `v` the "@type" of `type` unless it has one (protobuf
// reads "@type" anywhere in an Any).
void Type(json::Value &v, std::string_view type) {
  if (json::Find(v, "@type") == nullptr) {
    Add(*v.object(), "@type", json::Value{TypeUrl(type)});
  }
}

// Drops the object `v`'s "@type" where it is `type`'s.
void Untype(json::Value &v, std::string_view type) {
  const json::Value *t = json::Find(v, "@type");
  if (!type.empty() && t != nullptr && t->string() != nullptr && *t->string() == TypeUrl(type)) {
    json::Erase(v, "@type");
  }
}

struct ArgList {
  std::string_view list;   // "modules" / "ports"
  std::string_view kind;   // the member naming the class: "mclass" / "driver"
  std::string_view what;   // "module" / "port"
  std::string_view noun;   // "module class" / "port driver"
};
constexpr ArgList kModules{"modules", "mclass", "module", "module class"};
constexpr ArgList kPorts{"ports", "driver", "port", "port driver"};

std::string Label(const json::Value &item) {
  const json::Value *name = json::Find(item, "name");
  return name != nullptr && name->string() != nullptr ? *name->string() : "?";
}

// Gives each element's "arg" its implied "@type" where it has none.
std::expected<void, std::string> FillTypes(json::Value &doc, const ArgList &l,
                                           const TypeMap &types) {
  json::Value *items = json::Find(doc, l.list);
  if (items == nullptr) {
    return {};
  }
  json::Array *array = items->array();
  if (array == nullptr) {
    return std::unexpected(std::format("\"{}\" is not a list", l.list));
  }
  for (json::Value &item : *array) {
    json::Value *arg = json::Find(item, "arg");
    if (arg == nullptr || arg->object() == nullptr || json::Find(*arg, "@type") != nullptr) {
      continue;  // protobuf's parser reports a non-object arg
    }
    const json::Value *kind = json::Find(item, l.kind);
    if (kind == nullptr || kind->string() == nullptr) {
      return std::unexpected(
          std::format("{} \"{}\": an \"arg\" needs \"{}\"", l.what, Label(item), l.kind));
    }
    const auto it = types.find(*kind->string());
    if (it == types.end()) {
      return std::unexpected(std::format("{} \"{}\": the daemon has no {} \"{}\"", l.what,
                                         Label(item), l.noun, *kind->string()));
    }
    if (it->second.empty()) {
      return std::unexpected(std::format(
          "{} \"{}\": {} \"{}\" declares no argument type; give \"@type\" in \"arg\"", l.what,
          Label(item), l.noun, *kind->string()));
    }
    Type(*arg, it->second);
  }
  return {};
}

// Drops each element's "@type" where it is the implied one.
void DropImpliedTypes(json::Value &doc, const ArgList &l, const TypeMap &types) {
  json::Value *items = json::Find(doc, l.list);
  if (items == nullptr || items->array() == nullptr) {
    return;
  }
  for (json::Value &item : *items->array()) {
    json::Value *arg = json::Find(item, "arg");
    const json::Value *kind = json::Find(item, l.kind);
    if (arg == nullptr || kind == nullptr || kind->string() == nullptr) {
      continue;
    }
    if (const auto it = types.find(*kind->string()); it != types.end()) {
      Untype(*arg, it->second);
    }
  }
}

// The class of each module `doc` lists, by name.
TypeMap ModuleClasses(const json::Value &doc) {
  TypeMap classes;
  const json::Value *modules = json::Find(doc, "modules");
  if (modules == nullptr || modules->array() == nullptr) {
    return classes;
  }
  for (const json::Value &m : *modules->array()) {
    const json::Value *name = json::Find(m, "name");
    const json::Value *mclass = json::Find(m, "mclass");
    if (name != nullptr && name->string() != nullptr && mclass != nullptr &&
        mclass->string() != nullptr) {
      classes.emplace(*name->string(), *mclass->string());
    }
  }
  return classes;
}

// The table the resource `name`, "<module>/<table>", is: the one its module's
// class declares, the module being the pipeline's of that name (as the
// daemon resolves it).
std::expected<const pb::ResourceSchema *, std::string> TableOf(std::string_view name,
                                                                const TypeMap &classes,
                                                                const Schema::Tables &tables) {
  const size_t slash = name.rfind('/');
  if (slash == std::string_view::npos) {
    return std::unexpected(std::format(
        "resource \"{}\": a resource is named \"<module>/<table>\"", name));
  }
  const std::string_view module = name.substr(0, slash);
  const std::string_view table = name.substr(slash + 1);
  const auto mclass = classes.find(module);
  if (mclass == classes.end()) {
    return std::unexpected(
        std::format("resource \"{}\": no module \"{}\" in the pipeline", name, module));
  }
  std::string known;
  if (const auto c = tables.find(mclass->second); c != tables.end()) {
    if (const auto t = c->second.find(table); t != c->second.end()) {
      return &t->second;
    }
    for (const auto &[t, _] : c->second) {
      known += (known.empty() ? "" : ", ") + t;
    }
  }
  return std::unexpected(std::format("resource \"{}\": module class {} has no table \"{}\" ({})",
                                     name, mclass->second, table,
                                     known.empty() ? "it has none" : "it has: " + known));
}

// Rewrites the manifest's "resources", {name: contents}, as Pipeline's,
// [{"name": name, "entries": [{"key": {}, "value": {}}]}], each key and value
// given its table's type unless it has one. A map resource's contents are its
// entries, a singleton's its value (an entry without a key).
std::expected<void, std::string> ResourcesToPipeline(json::Value &doc,
                                                     const Schema::Tables &tables) {
  json::Value *resources = json::Find(doc, "resources");
  if (resources == nullptr) {
    return {};
  }
  if (resources->object() == nullptr) {
    return std::unexpected(std::string("\"resources\" is not an object of resource names"));
  }
  const TypeMap classes = ModuleClasses(doc);
  json::Array listed;
  for (auto &[name, contents] : *resources->object()) {
    auto table = TableOf(name, classes, tables);
    if (!table) {
      return std::unexpected(table.error());
    }
    const pb::ResourceSchema &t = **table;
    json::Value entries{json::Array{}};
    if (t.kind() == pb::TransactionResource::KIND_SINGLETON) {
      if (contents.object() == nullptr) {
        return std::unexpected(std::format(
            "resource \"{}\": a singleton; give its value, an object ({})", name, t.value_type()));
      }
      Type(contents, t.value_type());
      json::Value entry{json::Object{}};
      Add(*entry.object(), "value", std::move(contents));
      entries.array()->push_back(std::move(entry));
    } else {
      if (contents.array() == nullptr) {
        return std::unexpected(std::format("resource \"{}\": not a list of entries", name));
      }
      for (size_t i = 0; i < contents.array()->size(); i++) {
        json::Value &entry = (*contents.array())[i];
        json::Value *key = json::Find(entry, "key");
        json::Value *value = json::Find(entry, "value");
        if (key == nullptr || value == nullptr || entry.object()->size() != 2 ||
            key->object() == nullptr || value->object() == nullptr) {
          return std::unexpected(std::format(
              "resource \"{}\" entry {}: needs exactly \"key\" and \"value\", both objects", name,
              i));
        }
        Type(*key, t.key_type());
        Type(*value, t.value_type());
      }
      entries = std::move(contents);
    }
    json::Value resource{json::Object{}};
    Add(*resource.object(), "name", json::Value{name});
    Add(*resource.object(), "entries", std::move(entries));
    listed.push_back(std::move(resource));
  }
  resources->v = std::move(listed);
  return {};
}

// The reverse, for a pipeline read back: Pipeline's "resources" as {name:
// contents}, "@type" dropped where it is the table's. A resource bessctl
// cannot place in a table keeps its entries as they are.
void ResourcesFromPipeline(json::Value &doc, const Schema::Tables &tables) {
  json::Value *resources = json::Find(doc, "resources");
  if (resources == nullptr || resources->array() == nullptr) {
    return;
  }
  const TypeMap classes = ModuleClasses(doc);
  json::Object named;
  for (json::Value &resource : *resources->array()) {
    const json::Value *name = json::Find(resource, "name");
    if (name == nullptr || name->string() == nullptr) {
      continue;  // the daemon names every resource it lists
    }
    json::Value *listed = json::Find(resource, "entries");  // absent when empty
    json::Value entries = listed != nullptr ? std::move(*listed) : json::Value{json::Array{}};
    auto table = TableOf(*name->string(), classes, tables);
    if (table && entries.array() != nullptr) {
      const pb::ResourceSchema &t = **table;
      json::Array &list = *entries.array();
      if (t.kind() == pb::TransactionResource::KIND_SINGLETON) {
        json::Value *value = list.size() == 1 ? json::Find(list[0], "value") : nullptr;
        if (value != nullptr && value->object() != nullptr) {
          Untype(*value, t.value_type());
          Add(named, *name->string(), std::move(*value));
          continue;
        }
      } else {
        for (json::Value &entry : list) {
          for (const auto &[member, type] :
               {std::pair{"key", &t.key_type()}, std::pair{"value", &t.value_type()}}) {
            if (json::Value *v = json::Find(entry, member); v != nullptr && v->object() != nullptr) {
              Untype(*v, *type);
            }
          }
        }
      }
    }
    Add(named, *name->string(), std::move(entries));
  }
  resources->v = std::move(named);
}

}  // namespace

std::expected<std::unique_ptr<Schema>, std::string> Schema::From(
    const pb::GetSchemaResponse &schema) {
  std::unique_ptr<Schema> s(new Schema());
  s->pool_ = std::make_unique<gp::DescriptorPool>();
  for (const gp::FileDescriptorProto &file : schema.descriptors().file()) {
    if (s->pool_->BuildFile(file) == nullptr) {
      return std::unexpected(std::format("the daemon's schema: cannot load {}", file.name()));
    }
  }
  if (!AddGenerated(pb::Pipeline::descriptor()->file(), *s->pool_)) {
    return std::unexpected(std::string("the daemon's schema conflicts with control_v2.proto"));
  }
  s->factory_ = std::make_unique<gp::DynamicMessageFactory>(s->pool_.get());
  s->resolver_.reset(
      gp::util::NewTypeResolverForDescriptorPool(std::string(kUrlPrefix), s->pool_.get()));
  for (const pb::ModuleClassSchema &c : schema.module_classes()) {
    s->module_types_.emplace(c.name(), c.init_arg_type());
    auto &actions = s->actions_[c.name()];
    for (const pb::ActionSchema &a : c.actions()) {
      actions.emplace(a.name(), a.arg_type());
    }
    auto &tables = s->tables_[c.name()];
    for (const pb::ResourceSchema &r : c.resources()) {
      tables.emplace(r.table(), r);
    }
  }
  for (const pb::PortDriverSchema &d : schema.port_drivers()) {
    s->driver_types_.emplace(d.name(), d.init_arg_type());
  }
  return s;
}

Format FormatOf(std::string_view path, std::string_view text) {
  if (path.ends_with(".json")) {
    return Format::kJson;
  }
  if (path.ends_with(".yaml") || path.ends_with(".yml")) {
    return Format::kYaml;
  }
  const size_t first = text.find_first_not_of(" \t\r\n");
  return first != std::string_view::npos && text[first] == '{' ? Format::kJson : Format::kYaml;
}

std::expected<pb::Pipeline, std::string> Schema::PipelineFrom(std::string_view text,
                                                              Format format) const {
  auto doc = format == Format::kJson ? json::Parse(text) : yaml::Parse(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  if (doc->object() == nullptr) {
    return std::unexpected(std::string("a manifest is an object (a mapping)"));
  }
  if (auto listed = ResourcesToPipeline(*doc, tables_); !listed) {
    return std::unexpected(listed.error());
  }
  for (const auto &[list, types] :
       {std::pair{&kModules, &module_types_}, std::pair{&kPorts, &driver_types_}}) {
    if (auto filled = FillTypes(*doc, *list, *types); !filled) {
      return std::unexpected(filled.error());
    }
  }
  std::string binary;
  const auto status = gp::util::JsonToBinaryString(
      resolver_.get(), TypeUrl(pb::Pipeline::descriptor()->full_name()), json::Dump(*doc),
      &binary, gp::util::JsonParseOptions());
  if (!status.ok()) {
    return std::unexpected(ParseError(Message(status)));
  }
  pb::Pipeline p;
  if (!p.ParseFromString(binary)) {
    return std::unexpected(std::string("the manifest did not encode as a Pipeline"));
  }
  return p;
}

std::expected<std::string, std::string> Schema::PipelineTo(const pb::Pipeline &p,
                                                         Format format) const {
  gp::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string text;
  const auto status = gp::util::BinaryToJsonString(
      resolver_.get(), TypeUrl(pb::Pipeline::descriptor()->full_name()), p.SerializeAsString(),
      &text, options);
  if (!status.ok()) {
    return std::unexpected(Message(status));
  }
  auto doc = json::Parse(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  DropImpliedTypes(*doc, kModules, module_types_);
  DropImpliedTypes(*doc, kPorts, driver_types_);
  ResourcesFromPipeline(*doc, tables_);
  return format == Format::kJson ? json::Dump(*doc, 2) : yaml::Dump(*doc);
}

std::expected<std::unique_ptr<gp::Message>, std::string> Schema::MessageFromJson(
    std::string_view type, std::string_view json) const {
  const gp::Descriptor *d = pool_->FindMessageTypeByName(std::string(type));
  if (d == nullptr) {
    return std::unexpected(std::format("the daemon's schema has no type {}", type));
  }
  std::unique_ptr<gp::Message> m(factory_->GetPrototype(d)->New());
  const auto status = gp::util::JsonStringToMessage(std::string(json), m.get());
  if (!status.ok()) {
    return std::unexpected(ParseError(Message(status)));
  }
  return m;
}

std::expected<std::string, std::string> Schema::ActionArgType(std::string_view mclass,
                                                            std::string_view action) const {
  const auto c = actions_.find(mclass);
  if (c == actions_.end()) {
    return std::unexpected(std::format("the daemon has no module class {}", mclass));
  }
  const auto a = c->second.find(action);
  if (a == c->second.end()) {
    std::string known;
    for (const auto &[name, _] : c->second) {
      known += (known.empty() ? "" : ", ") + name;
    }
    return std::unexpected(std::format("{} has no action {} ({})", mclass, action,
                                       known.empty() ? "it has none" : "it has: " + known));
  }
  if (a->second.empty()) {
    return std::unexpected(
        std::format("{} {}: the daemon declares no argument type for it", mclass, action));
  }
  return a->second;
}

std::expected<std::unique_ptr<gp::Message>, std::string> Schema::MessageFromText(
    std::string_view type, std::string_view text) const {
  // JSON when it parses as JSON, else YAML (a flow mapping such as
  // {ip: 1.2.3.4} starts with "{" too).
  if (json::Parse(text)) {
    return MessageFromJson(type, text);
  }
  auto doc = yaml::Parse(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  return MessageFromJson(type, json::Dump(*doc));
}

std::expected<std::string, std::string> Schema::AnyTo(const gp::Any &any, Format format) const {
  gp::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string text;
  const auto status =
      gp::util::BinaryToJsonString(resolver_.get(), any.type_url(), any.value(), &text, options);
  if (!status.ok()) {
    return std::unexpected(std::format("a result of type {}: {}", any.type_url(), Message(status)));
  }
  auto doc = json::Parse(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  return format == Format::kJson ? json::Dump(*doc, 2) : yaml::Dump(*doc);
}

}  // namespace bessctl
