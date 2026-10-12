// SPDX-License-Identifier: BSD-3-Clause

// Manifests: a control_v2 Pipeline in protobuf's JSON mapping, with one
// convenience: a module's or port's "arg" may leave out "@type", which the
// class's or driver's init type from the daemon's schema (GetSchema) fills
// in. Reading a pipeline back drops "@type" where it is that implied type,
// so `bessctl get` prints what `bessctl apply` reads.

#ifndef BESSCTL_MANIFEST_H_
#define BESSCTL_MANIFEST_H_

#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/type_resolver.h>

#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "bess/sdk/client.h"

namespace bessctl {

namespace pb = ::bess::pb::v2;

class Schema {
 public:
  // The daemon's types (`schema`) plus control_v2's own.
  static std::expected<std::unique_ptr<Schema>, std::string> From(
      const pb::GetSchemaResponse &schema);

  std::expected<pb::Pipeline, std::string> PipelineFromJson(std::string_view text) const;
  // Pretty JSON, field names as in the .proto files.
  std::expected<std::string, std::string> PipelineToJson(const pb::Pipeline &p) const;

  // A message of the daemon's type `type`, as JSON (for commands, resources).
  std::expected<std::string, std::string> MessageToJson(std::string_view type,
                                                        const std::string &binary) const;

  const pb::GetSchemaResponse &response() const { return response_; }

 private:
  Schema() = default;

  pb::GetSchemaResponse response_;
  std::unique_ptr<google::protobuf::DescriptorPool> pool_;
  std::unique_ptr<google::protobuf::util::TypeResolver> resolver_;
  std::map<std::string, std::string, std::less<>> module_types_;  // class -> init type
  std::map<std::string, std::string, std::less<>> driver_types_;  // driver -> init type
};

}  // namespace bessctl

#endif  // BESSCTL_MANIFEST_H_
