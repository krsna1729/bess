// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_METRICS_EXPOSITION_H_
#define BESS_CONTROL_METRICS_EXPOSITION_H_

#include <string>

#include "pb/control_v2.pb.h"

namespace bess::control {

// A ListMetrics answer in the Prometheus text exposition format, version
// 0.0.4 (D-125): what bessd serves at GET /metrics with --http_addr.
//
// Each family (samples of one name) is preceded by its "# HELP" and "# TYPE"
// lines, once; families appear in name order, a family's samples in the
// answer's order (by labels). KIND_COUNTER is "counter", KIND_GAUGE
// "gauge", anything else "untyped". Labels are written in key order; label
// values escape backslash, double quote and newline, help text backslash
// and newline. Values are the shortest text that reads back as the same
// double ("NaN", "+Inf", "-Inf" as the format spells them). Last, the gauge
// bess_daemon_epoch: counters restart when it changes.
std::string MetricsExposition(const pb::v2::ListMetricsResponse &response);

}  // namespace bess::control

#endif  // BESS_CONTROL_METRICS_EXPOSITION_H_
