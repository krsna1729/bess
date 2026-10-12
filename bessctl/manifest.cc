// SPDX-License-Identifier: BSD-3-Clause

#include "manifest.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/type_resolver_util.h>

#include <format>
#include <regex>

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
    // Appended, not inserted first: protobuf reads "@type" anywhere in an Any,
    // and inserting moved every member through a temporary that GCC 14 at -O3
    // flagged as maybe-uninitialized (a false positive).
    json::Object &members = *arg->object();
    members.push_back({"@type", json::Value{}});
    members.back().value.v = TypeUrl(it->second);
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
    const json::Value *type = json::Find(*arg, "@type");
    const auto it = types.find(*kind->string());
    if (type != nullptr && type->string() != nullptr && it != types.end() &&
        !it->second.empty() && *type->string() == TypeUrl(it->second)) {
      json::Erase(*arg, "@type");
    }
  }
}

// Takes "resources" out of the manifest `doc`: {name: [{"key": {}, "value": {}}]}.
std::expected<std::vector<std::pair<std::string, std::vector<Manifest::Entry>>>, std::string>
TakeResources(json::Value &doc) {
  std::vector<std::pair<std::string, std::vector<Manifest::Entry>>> out;
  const json::Value *resources = json::Find(doc, "resources");
  if (resources == nullptr) {
    return out;
  }
  if (resources->object() == nullptr) {
    return std::unexpected(std::string("\"resources\" is not an object of resource names"));
  }
  for (const auto &[name, list] : *resources->object()) {
    if (list.array() == nullptr) {
      return std::unexpected(std::format("resource \"{}\": not a list of entries", name));
    }
    std::vector<Manifest::Entry> entries;
    for (size_t i = 0; i < list.array()->size(); i++) {
      const json::Value &entry = (*list.array())[i];
      const json::Value *key = json::Find(entry, "key");
      const json::Value *value = json::Find(entry, "value");
      if (key == nullptr || value == nullptr || entry.object()->size() != 2 ||
          key->object() == nullptr || value->object() == nullptr) {
        return std::unexpected(std::format(
            "resource \"{}\" entry {}: needs exactly \"key\" and \"value\", both objects", name, i));
      }
      entries.push_back({json::Dump(*key), json::Dump(*value)});
    }
    out.emplace_back(name, std::move(entries));
  }
  json::Erase(doc, "resources");
  return out;
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

std::expected<Manifest, std::string> Schema::ManifestFrom(std::string_view text,
                                                          Format format) const {
  auto doc = format == Format::kJson ? json::Parse(text) : yaml::Parse(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  if (doc->object() == nullptr) {
    return std::unexpected(std::string("a manifest is an object (a mapping)"));
  }
  Manifest m;
  auto resources = TakeResources(*doc);
  if (!resources) {
    return std::unexpected(resources.error());
  }
  m.resources = std::move(*resources);
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
  if (!m.pipeline.ParseFromString(binary)) {
    return std::unexpected(std::string("the manifest did not encode as a Pipeline"));
  }
  return m;
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

}  // namespace bessctl
