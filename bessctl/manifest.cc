// SPDX-License-Identifier: BSD-3-Clause

#include "manifest.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/type_resolver_util.h>

#include <format>
#include <regex>

#include "json.h"

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
    arg->object()->emplace(arg->object()->begin(), "@type", json::Value{TypeUrl(it->second)});
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

}  // namespace

std::expected<std::unique_ptr<Schema>, std::string> Schema::From(
    const pb::GetSchemaResponse &schema) {
  std::unique_ptr<Schema> s(new Schema());
  s->response_ = schema;
  s->pool_ = std::make_unique<gp::DescriptorPool>();
  for (const gp::FileDescriptorProto &file : schema.descriptors().file()) {
    if (s->pool_->BuildFile(file) == nullptr) {
      return std::unexpected(std::format("the daemon's schema: cannot load {}", file.name()));
    }
  }
  if (!AddGenerated(pb::Pipeline::descriptor()->file(), *s->pool_)) {
    return std::unexpected(std::string("the daemon's schema conflicts with control_v2.proto"));
  }
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

std::expected<pb::Pipeline, std::string> Schema::PipelineFromJson(std::string_view text) const {
  auto doc = json::Parse(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  if (doc->object() == nullptr) {
    return std::unexpected(std::string("a manifest is a JSON object"));
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

std::expected<std::string, std::string> Schema::PipelineToJson(const pb::Pipeline &p) const {
  auto text = MessageToJson(pb::Pipeline::descriptor()->full_name(), p.SerializeAsString());
  if (!text) {
    return text;
  }
  auto doc = json::Parse(*text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  DropImpliedTypes(*doc, kModules, module_types_);
  DropImpliedTypes(*doc, kPorts, driver_types_);
  return json::Dump(*doc, 2);
}

std::expected<std::string, std::string> Schema::MessageToJson(std::string_view type,
                                                              const std::string &binary) const {
  gp::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string out;
  const auto status =
      gp::util::BinaryToJsonString(resolver_.get(), TypeUrl(type), binary, &out, options);
  if (!status.ok()) {
    return std::unexpected(Message(status));
  }
  return out;
}

}  // namespace bessctl
