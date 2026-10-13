// SPDX-License-Identifier: BSD-3-Clause

// The Prometheus text format bessd serves at GET /metrics (D-125; was
// tools/bess_prometheus.py, D-076).

#include "control/metrics_exposition.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>
#include <string>

namespace bess::control {
namespace {

namespace v2 = pb::v2;

v2::MetricSample *Add(v2::ListMetricsResponse &r, const std::string &name,
                      v2::MetricSample::Kind kind, const std::string &help, double value,
                      const std::map<std::string, std::string> &labels = {}) {
  v2::MetricSample *s = r.add_samples();
  s->set_name(name);
  s->set_kind(kind);
  s->set_help(help);
  s->set_value(value);
  for (const auto &[k, v] : labels) {
    (*s->mutable_labels())[k] = v;
  }
  return s;
}

constexpr char kEpochTail[] =
    "# HELP bess_daemon_epoch Daemon epoch: counters restart when it changes\n"
    "# TYPE bess_daemon_epoch gauge\n";

TEST(MetricsExpositionTest, EmptyAnswerIsTheEpochAlone) {
  v2::ListMetricsResponse r;
  r.set_daemon_epoch(18446744073709551615u);  // printed exactly, not as a double
  EXPECT_EQ(std::string(kEpochTail) + "bess_daemon_epoch 18446744073709551615\n",
            MetricsExposition(r));
}

// HELP and TYPE once per family, before its first sample; families in name
// order whatever order the samples came in; a family's samples in theirs.
TEST(MetricsExpositionTest, FamiliesSortedWithHelpAndTypeFirst) {
  v2::ListMetricsResponse r;
  r.set_daemon_epoch(7);
  Add(r, "bess_z_gauge", v2::MetricSample::KIND_GAUGE, "A gauge", 3);
  Add(r, "bess_a_total", v2::MetricSample::KIND_COUNTER, "A counter", 1, {{"outcome", "applied"}});
  Add(r, "bess_m_untyped", v2::MetricSample::KIND_UNSPECIFIED, "No kind", 0);
  Add(r, "bess_a_total", v2::MetricSample::KIND_COUNTER, "A counter", 2, {{"outcome", "rejected"}});
  EXPECT_EQ(
      "# HELP bess_a_total A counter\n"
      "# TYPE bess_a_total counter\n"
      "bess_a_total{outcome=\"applied\"} 1\n"
      "bess_a_total{outcome=\"rejected\"} 2\n"
      "# HELP bess_m_untyped No kind\n"
      "# TYPE bess_m_untyped untyped\n"
      "bess_m_untyped 0\n"
      "# HELP bess_z_gauge A gauge\n"
      "# TYPE bess_z_gauge gauge\n"
      "bess_z_gauge 3\n" +
          std::string(kEpochTail) + "bess_daemon_epoch 7\n",
      MetricsExposition(r));
}

// Label values escape backslash, quote and newline; help text backslash and
// newline only. Labels are in key order (a protobuf map has none).
TEST(MetricsExpositionTest, EscapingAndLabelOrder) {
  v2::ListMetricsResponse r;
  Add(r, "bess_x", v2::MetricSample::KIND_GAUGE, "say \"hi\"\\\nbye", 1,
      {{"module", "a\"b\\c\nd"}, {"gate", "0"}, {"direction", "out"}});
  EXPECT_EQ(
      "# HELP bess_x say \"hi\"\\\\\\nbye\n"
      "# TYPE bess_x gauge\n"
      "bess_x{direction=\"out\",gate=\"0\",module=\"a\\\"b\\\\c\\nd\"} 1\n" +
          std::string(kEpochTail) + "bess_daemon_epoch 0\n",
      MetricsExposition(r));
}

TEST(MetricsExpositionTest, Values) {
  const auto rendered = [](double value) {
    v2::ListMetricsResponse r;
    Add(r, "v", v2::MetricSample::KIND_GAUGE, "", value);
    const std::string text = MetricsExposition(r);
    const size_t at = text.find("\nv ") + 3;
    return text.substr(at, text.find('\n', at) - at);
  };
  EXPECT_EQ("0", rendered(0));
  EXPECT_EQ("42", rendered(42));
  EXPECT_EQ("0.5", rendered(0.5));
  EXPECT_EQ("-1.25", rendered(-1.25));
  EXPECT_EQ("1e+20", rendered(1e20));
  EXPECT_EQ("1234567890123", rendered(1234567890123.0));  // a large count stays an integer
  EXPECT_EQ("NaN", rendered(std::nan("")));
  EXPECT_EQ("+Inf", rendered(std::numeric_limits<double>::infinity()));
  EXPECT_EQ("-Inf", rendered(-std::numeric_limits<double>::infinity()));
  // The shortest text that reads back as the same double.
  EXPECT_EQ(0.1, std::stod(rendered(0.1)));
  EXPECT_EQ("0.1", rendered(0.1));
}

}  // namespace
}  // namespace bess::control
