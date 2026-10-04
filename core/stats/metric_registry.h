// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_METRIC_REGISTRY_H_
#define BESS_STATS_METRIC_REGISTRY_H_

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Generic operational metrics, pulled (roadmap M25). Libraries keep their own
// plain stats structs and worker-local counters; whoever owns an instance (a
// module, the runtime) registers a callback that reads them when the control
// plane asks. Nothing here is on a packet path, and no exporter format
// (Prometheus or other) is known to a library: exporters translate samples
// outside (ListMetrics RPC, bessctl, tools/bess_prometheus.py).

namespace bess::stats {

enum class MetricKind : uint8_t { kCounter, kGauge };

// One value of one metric, with its labels (sorted by name).
struct MetricSample {
  std::string name;
  MetricKind kind = MetricKind::kGauge;
  std::string help;
  std::vector<std::pair<std::string, std::string>> labels;
  double value = 0;
};

// What a source writes into: Counter/Gauge append samples. Names follow
// Prometheus conventions (snake_case, `bess_` prefix, counters end in
// `_total`), so an exporter can pass them through unchanged.
class MetricWriter {
 public:
  void Counter(std::string name, std::string help, double value,
               std::vector<std::pair<std::string, std::string>> labels = {}) {
    Add(MetricKind::kCounter, std::move(name), std::move(help), value, std::move(labels));
  }
  void Gauge(std::string name, std::string help, double value,
             std::vector<std::pair<std::string, std::string>> labels = {}) {
    Add(MetricKind::kGauge, std::move(name), std::move(help), value, std::move(labels));
  }
  std::vector<MetricSample> &samples() { return samples_; }

 private:
  void Add(MetricKind kind, std::string name, std::string help, double value,
           std::vector<std::pair<std::string, std::string>> labels);
  std::vector<MetricSample> samples_;
};

class MetricRegistry;

// Owns one registration: destroying or resetting it removes the source, so a
// callback never outlives what it reads. Declare it after that object.
class MetricSource {
 public:
  MetricSource() = default;
  MetricSource(MetricSource &&other) noexcept
      : registry_(std::exchange(other.registry_, nullptr)), id_(other.id_) {}
  MetricSource &operator=(MetricSource &&other) noexcept {
    if (this != &other) {
      Reset();
      registry_ = std::exchange(other.registry_, nullptr);
      id_ = other.id_;
    }
    return *this;
  }
  MetricSource(const MetricSource &) = delete;
  MetricSource &operator=(const MetricSource &) = delete;
  ~MetricSource() { Reset(); }

  void Reset();
  bool registered() const { return registry_ != nullptr; }

 private:
  friend class MetricRegistry;
  MetricSource(MetricRegistry *registry, uint64_t id) : registry_(registry), id_(id) {}
  MetricRegistry *registry_ = nullptr;
  uint64_t id_ = 0;
};

// Thread-safe. Collect() calls every source under the registry's lock, on the
// caller's (control) thread; a source must not register or reset sources.
class MetricRegistry {
 public:
  MetricRegistry() = default;
  MetricRegistry(const MetricRegistry &) = delete;
  MetricRegistry &operator=(const MetricRegistry &) = delete;

  // `collect` appends this source's samples; it reads its object's stats.
  [[nodiscard]] MetricSource Register(std::function<void(MetricWriter &)> collect);

  // Every source's samples, sorted by name then labels.
  std::vector<MetricSample> Collect() const;
  size_t sources() const;

 private:
  friend class MetricSource;
  void Unregister(uint64_t id);

  mutable std::mutex mutex_;
  uint64_t next_id_ = 1;
  std::map<uint64_t, std::function<void(MetricWriter &)>> sources_;
};

}  // namespace bess::stats

#endif  // BESS_STATS_METRIC_REGISTRY_H_
