// SPDX-License-Identifier: BSD-3-Clause

// Route-table tests run once per backend (D-132): rte_lpm and rte_fib must
// give the same answers, so every route oracle is a value-parameterised suite
//
//   class RouteTableTest : public bess::testing::RouteBackendTest {};
//   TEST_P(RouteTableTest, Something) { auto config = TestRouteConfig(); ... }
//   BESS_INSTANTIATE_ROUTE_BACKENDS(RouteTableTest);
//
// TestRouteConfig() is a default LpmRouteTable::Config with the backend of
// the running test, so helpers outside the fixture (MakeTable, MakeRouter)
// need no extra parameter. Tests are named Backend/<Suite>.<Test>/Lpm or /Fib.

#ifndef BESS_TESTING_ROUTE_BACKENDS_H_
#define BESS_TESTING_ROUTE_BACKENDS_H_

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "route/route_table.h"

namespace bess::testing {

namespace internal {
inline route::RouteBackend &CurrentRouteBackend() {
  static route::RouteBackend backend = route::RouteBackend::kRteLpm;
  return backend;
}
}  // namespace internal

// The running test's backend; LPM outside a RouteBackendTest.
inline route::RouteBackend TestRouteBackend() {
  return internal::CurrentRouteBackend();
}

inline route::LpmRouteTable::Config TestRouteConfig() {
  route::LpmRouteTable::Config config;
  config.backend = TestRouteBackend();
  return config;
}

class RouteBackendTest : public ::testing::TestWithParam<route::RouteBackend> {
 protected:
  RouteBackendTest() { internal::CurrentRouteBackend() = GetParam(); }
  ~RouteBackendTest() override {
    internal::CurrentRouteBackend() = route::RouteBackend::kRteLpm;
  }
};

inline std::string RouteBackendName(
    const ::testing::TestParamInfo<route::RouteBackend> &info) {
  return info.param == route::RouteBackend::kRteFib ? "Fib" : "Lpm";
}

// The backends this CPU supports (route::RouteBackendSupported): rte_fib only
// on x86-64.
inline std::vector<route::RouteBackend> SupportedRouteBackends() {
  std::vector<route::RouteBackend> backends;
  for (const route::RouteBackend backend :
       {route::RouteBackend::kRteLpm, route::RouteBackend::kRteFib}) {
    if (route::RouteBackendSupported(backend)) {
      backends.push_back(backend);
    }
  }
  return backends;
}

}  // namespace bess::testing

#define BESS_INSTANTIATE_ROUTE_BACKENDS(Suite)                       \
  INSTANTIATE_TEST_SUITE_P(                                          \
      Backend, Suite,                                                \
      ::testing::ValuesIn(bess::testing::SupportedRouteBackends()), \
      bess::testing::RouteBackendName)

#endif  // BESS_TESTING_ROUTE_BACKENDS_H_
