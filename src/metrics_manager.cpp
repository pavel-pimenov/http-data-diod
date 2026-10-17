#include "metrics_manager.hpp"
#include "prometheus/counter.h"
#include <memory>
#include "prometheus/registry.h"
#include <string>
#include "prometheus/gauge.h"
#include "prometheus/histogram.h"
#include <vector>
#include "prometheus/family.h"

prometheus::Counter &MetricsManager::create_counter(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return create_single_metric<prometheus::Counter>(registry, name, help);
}

prometheus::Gauge &MetricsManager::create_gauge(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return create_single_metric<prometheus::Gauge>(registry, name, help);
}

prometheus::Histogram &MetricsManager::create_histogram(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help,
    const std::vector<double> &buckets) {
  return prometheus::BuildHistogram()
      .Name(name)
      .Help(help)
      .Register(*registry)
      .Add({}, buckets);
}

prometheus::Family<prometheus::Counter> &
MetricsManager::create_counter_family(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return create_metric_family<prometheus::Counter>(registry, name, help);
}

prometheus::Family<prometheus::Gauge> &
MetricsManager::create_gauge_family(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return create_metric_family<prometheus::Gauge>(registry, name, help);
}

prometheus::Family<prometheus::Histogram> &
MetricsManager::create_histogram_family(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return create_metric_family<prometheus::Histogram>(registry, name, help);
}
