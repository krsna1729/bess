// SPDX-License-Identifier: BSD-3-Clause

// Manifests: a control_v2 Pipeline in protobuf's JSON mapping, with two
// additions.
//  - A module's or port's "arg" may leave out "@type": the class's or
//    driver's init type from the daemon's schema (GetSchema) fills it in.
//    Reading a pipeline back drops "@type" where it is that implied type, so
//    `bessctl get` prints what `bessctl apply` reads.
//  - "resources" holds entries for transactional resources the pipeline's
//    modules create: {"em/rules": [{"key": {...}, "value": {...}}]}. They are
//    committed in one transaction after the pipeline is applied, since the
//    resources exist only then; a resource's key and value types come from
//    the daemon then too.

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
#include <utility>
#include <vector>

#include "bess/sdk/client.h"

namespace bessctl {

namespace pb = ::bess::pb::v2;

struct Manifest {
  // An entry's key and value, as JSON text (numbers as written).
  struct Entry {
    std::string key;
    std::string value;
  };

  pb::Pipeline pipeline;
  std::vector<std::pair<std::string, std::vector<Entry>>> resources;  // in file order

  size_t entries() const {
    size_t n = 0;
    for (const auto &[_, entries] : resources) {
      n += entries.size();
    }
    return n;
  }
};

class Schema {
 public:
  // The daemon's types (`schema`) plus control_v2's own.
  static std::expected<std::unique_ptr<Schema>, std::string> From(
      const pb::GetSchemaResponse &schema);

  std::expected<Manifest, std::string> ManifestFromJson(std::string_view text) const;
  // Pretty JSON, field names as in the .proto files.
  std::expected<std::string, std::string> PipelineToJson(const pb::Pipeline &p) const;

  // A message of the daemon's type `type` (a resource's key or value) from
  // JSON. Valid while this Schema lives.
  std::expected<std::unique_ptr<google::protobuf::Message>, std::string> MessageFromJson(
      std::string_view type, std::string_view json) const;

 private:
  Schema() = default;

  std::unique_ptr<google::protobuf::DescriptorPool> pool_;
  std::unique_ptr<google::protobuf::DynamicMessageFactory> factory_;
  std::unique_ptr<google::protobuf::util::TypeResolver> resolver_;
  std::map<std::string, std::string, std::less<>> module_types_;  // class -> init type
  std::map<std::string, std::string, std::less<>> driver_types_;  // driver -> init type
};

}  // namespace bessctl

#endif  // BESSCTL_MANIFEST_H_
