// SPDX-License-Identifier: BSD-3-Clause

#include "control/metrics_exposition.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace bess::control {
namespace {

namespace v2 = pb::v2;

// Backslash and newline always; the double quote only inside a label value.
void AppendEscaped(std::string &out, std::string_view text, bool quote) {
  for (const char c : text) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (quote && c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
}

void AppendValue(std::string &out, double value) {
  if (std::isnan(value)) {
    out += "NaN";
  } else if (std::isinf(value)) {
    out += value > 0 ? "+Inf" : "-Inf";
  } else {
    char buf[32];
    const auto done = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, done.ptr);
  }
}

const char *KindName(v2::MetricSample::Kind kind) {
  switch (kind) {
    case v2::MetricSample::KIND_COUNTER:
      return "counter";
    case v2::MetricSample::KIND_GAUGE:
      return "gauge";
    default:
      return "untyped";
  }
}

}  // namespace

std::string MetricsExposition(const v2::ListMetricsResponse &response) {
  // ListMetrics answers sorted already; sorting again (stably, by name)
  // keeps every family in one run whatever the source.
  std::vector<const v2::MetricSample *> samples;
  samples.reserve(static_cast<size_t>(response.samples_size()));
  for (const v2::MetricSample &s : response.samples()) {
    samples.push_back(&s);
  }
  std::stable_sort(samples.begin(), samples.end(),
                   [](const v2::MetricSample *a, const v2::MetricSample *b) {
                     return a->name() < b->name();
                   });

  std::string out;
  const std::string *family = nullptr;
  std::vector<std::pair<std::string_view, std::string_view>> labels;
  for (const v2::MetricSample *s : samples) {
    if (family == nullptr || *family != s->name()) {
      family = &s->name();
      out.append("# HELP ").append(s->name()).append(" ");
      AppendEscaped(out, s->help(), false);
      out.append("\n# TYPE ").append(s->name()).append(" ").append(KindName(s->kind()));
      out += '\n';
    }
    out += s->name();
    labels.clear();
    for (const auto &[key, value] : s->labels()) {
      labels.emplace_back(key, value);
    }
    if (!labels.empty()) {
      std::sort(labels.begin(), labels.end());
      out += '{';
      for (size_t i = 0; i < labels.size(); i++) {
        if (i != 0) {
          out += ',';
        }
        out.append(labels[i].first).append("=\"");
        AppendEscaped(out, labels[i].second, true);
        out += '"';
      }
      out += '}';
    }
    out += ' ';
    AppendValue(out, s->value());
    out += '\n';
  }
  out.append(
      "# HELP bess_daemon_epoch Daemon epoch: counters restart when it changes\n"
      "# TYPE bess_daemon_epoch gauge\n"
      "bess_daemon_epoch ");
  out.append(std::to_string(response.daemon_epoch()));
  out += '\n';
  return out;
}

}  // namespace bess::control
