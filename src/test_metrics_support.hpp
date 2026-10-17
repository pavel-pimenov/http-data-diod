#ifndef TEST_METRICS_SUPPORT_HPP
#define TEST_METRICS_SUPPORT_HPP

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <string>
#include <vector>

// Helpers for test fixtures that register whole bundles of Prometheus metrics:
// the fixtures differ only in the metric names, so the four-line
// prometheus::Build*() chain is folded into one call. Lives in its own header
// (instead of test_support.hpp) so dependency-light test TUs do not pull in the
// app context, request handler and stats logger just to register a counter.
// Test-only — the `test_` file-name prefix keeps the file out of the coverage
// gate.

namespace test_support {

[[nodiscard]] inline prometheus::Counter &make_counter(
    prometheus::Registry &registry, const std::string &name,
    const std::string &help) {
  return prometheus::BuildCounter()
      .Name(name)
      .Help(help)
      .Register(registry)
      .Add({});
}

[[nodiscard]] inline prometheus::Gauge &make_gauge(
    prometheus::Registry &registry, const std::string &name,
    const std::string &help) {
  return prometheus::BuildGauge()
      .Name(name)
      .Help(help)
      .Register(registry)
      .Add({});
}

// `buckets` mirrors the second argument of Family<Histogram>::Add: the default
// empty vector means "library default buckets", exactly like .Add({}) did.
[[nodiscard]] inline prometheus::Histogram &make_histogram(
    prometheus::Registry &registry, const std::string &name,
    const std::string &help, const std::vector<double> &buckets = {}) {
  return prometheus::BuildHistogram()
      .Name(name)
      .Help(help)
      .Register(registry)
      .Add({}, buckets);
}

} // namespace test_support

#endif
