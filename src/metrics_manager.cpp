#include "metrics_manager.hpp"

prometheus::Counter &MetricsManager::create_counter(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return prometheus::BuildCounter()
      .Name(name)
      .Help(help)
      .Register(*registry)
      .Add({});
}

prometheus::Gauge &MetricsManager::create_gauge(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return prometheus::BuildGauge().Name(name).Help(help).Register(*registry).Add(
      {});
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
  return prometheus::BuildCounter().Name(name).Help(help).Register(*registry);
}

prometheus::Family<prometheus::Gauge> &
MetricsManager::create_gauge_family(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help) {
  return prometheus::BuildGauge().Name(name).Help(help).Register(*registry);
}

prometheus::Family<prometheus::Histogram> &
MetricsManager::create_histogram_family(
    const std::shared_ptr<prometheus::Registry> &registry,
    const std::string &name, const std::string &help,
    const std::vector<double> &buckets) {
  return prometheus::BuildHistogram()
      .Name(name)
      .Help(help)
      .Register(*registry);
}