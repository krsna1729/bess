// SPDX-License-Identifier: BSD-3-Clause

#include "stats/metric_registry.h"

#include <algorithm>

namespace bess::stats {

void MetricWriter::Add(MetricKind kind, std::string name, std::string help, double value,
                       std::vector<std::pair<std::string, std::string>> labels) {
  std::sort(labels.begin(), labels.end());
  samples_.push_back({std::move(name), kind, std::move(help), std::move(labels), value});
}

void MetricSource::Reset() {
  if (registry_ != nullptr) {
    registry_->Unregister(id_);
    registry_ = nullptr;
  }
}

MetricSource MetricRegistry::Register(std::function<void(MetricWriter &)> collect) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t id = next_id_++;
  sources_.emplace(id, std::move(collect));
  return MetricSource(this, id);
}

void MetricRegistry::Unregister(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  sources_.erase(id);
}

std::vector<MetricSample> MetricRegistry::Collect() const {
  MetricWriter writer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[id, collect] : sources_) {
      collect(writer);
    }
  }
  auto &samples = writer.samples();
  std::stable_sort(samples.begin(), samples.end(), [](const auto &a, const auto &b) {
    return a.name != b.name ? a.name < b.name : a.labels < b.labels;
  });
  return std::move(samples);
}

size_t MetricRegistry::sources() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sources_.size();
}

}  // namespace bess::stats
