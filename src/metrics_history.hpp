#ifndef METRICS_HISTORY_HPP
#define METRICS_HISTORY_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "logger.hpp"

#include <prometheus/client_metric.h>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/metric_family.h>
#include <prometheus/registry.h>
#if __has_include(<generator>)
#include <generator>
#endif

// In-process, request-independent ring buffer of recent metric samples so the
// /stats page can draw small "activity" sparklines (Grafana-like) without ever
// querying VictoriaMetrics. A single background sampler thread snapshots the
// registry every `interval`; the /stats handler only reads the buffer, so the
// cost is independent of how often the page is opened. Cost: one Collect() per
// interval per process, bounded memory (max_samples x max_series_per_family).
//
// For counters and histogram/summary counts the sparkline shows the per-second
// rate (delta / elapsed); gauges are plotted as raw values.

namespace {

inline std::string mh_format_labels(
    const std::vector<prometheus::ClientMetric::Label> &labels) {
  if (labels.empty()) {
    return "";
  }
  std::string s = "{";
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i > 0) {
      s += ", ";
    }
    s += labels[i].name + "=" + labels[i].value;
  }
  s += "}";
  return s;
}

} // namespace

class MetricsHistory {
public:
  // Outcome of one sampling round: "ok", "error" (the collection threw) or
  // "no_registry". The list is the single source of truth: the AppContext
  // pre-creates a series per outcome.
  static constexpr std::array<const char *, 3> g_sample_outcomes = {"ok", "error",
                                                                 "no_registry"};

  // The collector is injectable so the failure path is testable: a registry
  // that throws must be counted, not fatal.
  using Collector = std::function<std::vector<prometheus::MetricFamily>()>;

  explicit MetricsHistory(
      std::shared_ptr<prometheus::Registry> registry,
      std::chrono::seconds interval = std::chrono::seconds(15),
      std::size_t max_series_per_family = 8,
      std::size_t max_samples = 240,
      prometheus::Family<prometheus::Counter> *samples = nullptr,
      Collector collector = {})
      : m_config{std::move(registry),
                 interval,
                 max_series_per_family,
                 max_samples},
        m_samples(samples),
        m_collector(std::move(collector)) {}

  ~MetricsHistory() { stop(); }

  void start() {
    if (m_sampler.m_thread.joinable()) {
      return;
    }
    m_sampler.m_thread = std::jthread(
        [this](const std::stop_token &st) { run(st); });
  }

  void stop() {
    if (m_sampler.m_thread.joinable()) {
      m_sampler.m_thread.request_stop();
      m_sampler.m_thread.join();
    }
  }

  struct Series {
    std::string m_labels;
    std::vector<std::pair<std::time_t, double>> m_points;
  };

  bool has_family(const std::string &family) const {
    const std::scoped_lock lk(m_store.m_mutex);
    return m_store.m_data.contains(family);
  }

  // Returns up to `limit` series (insertion order) for a metric family.
  std::vector<Series> get_series(const std::string &family,
                                 std::size_t limit) const {
    std::vector<Series> out;
    const std::scoped_lock lk(m_store.m_mutex);
    const auto it = m_store.m_data.find(family);
    if (it == m_store.m_data.end()) {
      return out;
    }
    for (const auto &kv : it->second) {
      if (out.size() >= limit) {
        break;
      }
      Series s;
      s.m_labels = kv.first;
      s.m_points.assign(kv.second.begin(), kv.second.end());
      out.push_back(std::move(s));
    }
    return out;
  }

#if __has_include(<generator>)
  // C++23 generator: lazy iteration over series, no vector allocation until consumed
  std::generator<Series> series_view(const std::string &family, std::size_t limit) const {
    const std::scoped_lock lk(m_store.m_mutex);
    const auto it = m_store.m_data.find(family);
    if (it == m_store.m_data.end()) {
      co_return;
    }
    std::size_t n = 0;
    for (const auto &kv : it->second) {
      if (n++ >= limit) {
        break;
      }
      Series s;
      s.m_labels = kv.first;
      s.m_points.assign(kv.second.begin(), kv.second.end());
      co_yield s;
    }
    co_return;
  }
#endif

private:
  prometheus::Family<prometheus::Counter> *m_samples = nullptr;
  Collector m_collector;

  void count_sample(const char *outcome) {
    if (m_samples != nullptr) {
      m_samples->Add({{"outcome", outcome}}).Increment();
    }
  }

  void run(std::stop_token st) {
    while (!st.stop_requested()) {
      sample();
      std::unique_lock lk(m_sampler.m_cv_mutex);
      m_sampler.m_cv.wait_for(lk, st, m_config.m_interval,
                              [&] { return st.stop_requested(); });
    }
  }

  void sample() {
    if (!m_config.m_registry) {
      count_sample("no_registry");
      return;
    }
    std::vector<prometheus::MetricFamily> families;
    try {
      families = m_collector ? m_collector() : m_config.m_registry->Collect();
    } catch (const std::exception &e) {
      // A registry that throws would otherwise take the whole process down
      // (an uncaught exception in the sampler thread calls std::terminate).
      Logger::error("Metrics history sampling failed: {}", e.what());
      count_sample("error");
      return;
    }
    count_sample("ok");
    const std::time_t now = std::time(nullptr);
    // span<const MetricFamily> — non-owning view над families, 0 копий
    const std::span<const prometheus::MetricFamily> fam_view(families);
    const std::scoped_lock lk(m_store.m_mutex);
    for (const auto &family : fam_view) {
      if (family.metric.empty()) {
        continue;
      }
      auto &smap = m_store.m_data[family.name];
      std::size_t stored = 0;
      const std::span<const prometheus::ClientMetric> view(family.metric);
      for (const auto &metric : view) {
        if (stored >= m_config.m_max_series_per_family) {
          break;
        }
        const std::string key = mh_format_labels(metric.label);
        const double val = extract_value(metric, family.type);
        auto &buf = smap[key];
        if (buf.empty() || buf.back().first != now) {
          buf.emplace_back(now, val);
          if (buf.size() > m_config.m_max_samples) {
            buf.pop_front();
          }
        } else {
          buf.back().second = val;
        }
        ++stored;
      }
    }
  }

  static double extract_value(const prometheus::ClientMetric &m,
                              prometheus::MetricType t) {
    switch (t) {
      case prometheus::MetricType::Counter:
        return m.counter.value;
      case prometheus::MetricType::Gauge:
        return m.gauge.value;
      case prometheus::MetricType::Histogram:
        return static_cast<double>(m.histogram.sample_count);
      case prometheus::MetricType::Summary:
        return static_cast<double>(m.summary.sample_count);
      default:
        return 0.0;
    }
  }

  struct Config {
    std::shared_ptr<prometheus::Registry> m_registry;
    std::chrono::seconds m_interval;
    std::size_t m_max_series_per_family;
    std::size_t m_max_samples;
  } m_config;
  struct Store {
    mutable std::mutex m_mutex;
    std::map<std::string,
             std::map<std::string,
                      std::deque<std::pair<std::time_t, double>>>>
        m_data;
  } m_store;
  struct Sampler {
    std::jthread m_thread;
    std::mutex m_cv_mutex;
    std::condition_variable_any m_cv;
  } m_sampler;
};

#endif // METRICS_HISTORY_HPP
