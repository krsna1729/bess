// SPDX-License-Identifier: BSD-3-Clause

// Manifests: a control_v2 Pipeline in protobuf's JSON mapping, written as
// JSON or YAML (yaml_io.h: the same tree, so the same meaning), with two
// additions.
//  - A module's or port's "arg" may leave out "@type": the class's or
//    driver's init type from the daemon's schema (GetSchema) fills it in.
//    Reading a pipeline back drops "@type" where it is that implied type, so
//    `bessctl get` prints what `bessctl apply` reads.
//  - "resources" (Pipeline.resources, D-129) is an object keyed by resource
//    name, "<module>/<table>": a map resource's entries as a list,
//    {"em/rules": [{"key": {...}, "value": {...}}]} ([] clears it), a
//    singleton as its value, {"em/settings": {"default_gate": 3}}. Its kind
//    and key and value types are the table's that the module's class
//    declares (GetSchema), the module being the manifest's of that name, so
//    a key or value may leave out "@type" too; reading back drops it again.

#ifndef BESSCTL_MANIFEST_H_
#define BESSCTL_MANIFEST_H_

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/type_resolver.h>

#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "bess/sdk/client.h"

namespace bessctl {

namespace pb = ::bess::pb::v2;

enum class Format { kJson, kYaml };

// A file's format: by extension (.json; .yaml, .yml), else JSON if its first
// non-blank character is "{", else YAML.
Format FormatOf(std::string_view path, std::string_view text);

class Schema {
 public:
  // The daemon's types (`schema`) plus control_v2's own.
  static std::expected<std::unique_ptr<Schema>, std::string> From(
      const pb::GetSchemaResponse &schema);

  // A manifest's pipeline, its resources' contents included.
  std::expected<pb::Pipeline, std::string> PipelineFrom(std::string_view text,
                                                        Format format) const;
  // Pretty JSON or block YAML, field names as in the .proto files.
  std::expected<std::string, std::string> PipelineTo(const pb::Pipeline &p, Format format) const;

  // A message of the daemon's type `type` from JSON. Valid while this Schema
  // lives.
  std::expected<std::unique_ptr<google::protobuf::Message>, std::string> MessageFromJson(
      std::string_view type, std::string_view json) const;

  // Module actions (D-129). The argument type of class `mclass`'s action
  // `action`, as GetSchema declares it.
  std::expected<std::string, std::string> ActionArgType(std::string_view mclass,
                                                        std::string_view action) const;
  // An action's argument from JSON text, or else YAML text.
  std::expected<std::unique_ptr<google::protobuf::Message>, std::string> MessageFromText(
      std::string_view type, std::string_view text) const;
  // An action's result in `format`, field names as in the .proto files.
  std::expected<std::string, std::string> AnyTo(const google::protobuf::Any &any,
                                                Format format) const;

  // class -> table -> its resource table (GetSchema's ResourceSchema)
  using Tables =
      std::map<std::string, std::map<std::string, pb::ResourceSchema, std::less<>>, std::less<>>;

 private:
  Schema() = default;

  std::unique_ptr<google::protobuf::DescriptorPool> pool_;
  std::unique_ptr<google::protobuf::DynamicMessageFactory> factory_;
  std::unique_ptr<google::protobuf::util::TypeResolver> resolver_;
  std::map<std::string, std::string, std::less<>> module_types_;  // class -> init type
  std::map<std::string, std::string, std::less<>> driver_types_;  // driver -> init type
  // class -> action -> argument type
  std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> actions_;
  Tables tables_;
};

}  // namespace bessctl

#endif  // BESSCTL_MANIFEST_H_
