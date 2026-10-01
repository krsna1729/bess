// SPDX-License-Identifier: BSD-3-Clause

#include "framework/resource_bindings.h"

namespace bess::framework {

void ResourceBinding::Reset() {
  if (owner_ != nullptr) {
    owner_->Unbind(resource_);
    owner_ = nullptr;
    resource_ = nullptr;
  }
}

ResourceBinding ResourceBindings::Bind(
    const dataplane::Resource &resource,
    std::shared_ptr<const ResourceCodec> codec) {
  codecs_.insert_or_assign(&resource, std::move(codec));
  return ResourceBinding(this, &resource);
}

const ResourceCodec *ResourceBindings::Find(
    const dataplane::Resource &resource) const {
  auto it = codecs_.find(&resource);
  return it == codecs_.end() ? nullptr : it->second.get();
}

void ResourceBindings::Unbind(const dataplane::Resource *resource) {
  codecs_.erase(resource);
}

ResourceBindings &ResourceBindings::ProcessDefault() {
  // Never destroyed: module destructors unbind during runtime teardown.
  static ResourceBindings *const bindings = new ResourceBindings;
  return *bindings;
}

}  // namespace bess::framework
