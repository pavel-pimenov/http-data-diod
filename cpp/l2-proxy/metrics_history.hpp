#ifndef METRICS_HISTORY_HPP
#define METRICS_HISTORY_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>
#include <prometheus/registry.h>

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
  explicit MetricsHistory(
      std::shared_ptr<prometheus::Registry> registry,
      std::chrono::seconds interval = std::chrono::seconds(15),
      std::size_t max_series_per_family = 8,
      std::size_t max_samples = 240)
      : m_registry(std::move(registry)),
        m_interval(interval),
        m_max_series_per_family(max_series_per_family),
        m_max_samples(max_samples) {}

  ~MetricsHistory() { stop(); }

  void start() {
    if (m_thread.joinable()) {
      return;
    }
    m_thread = std::jthread([this](std::stop_token st) { run(st); });
  }

  void stop() {
    if (m_thread.joinable()) {
      m_thread.request_stop();
      m_thread.join();
    }
  }

  struct Series {
    std::string labels;
    std::vector<std::pair<std::time_t, double>> points;
  };

  bool has_family(const std::string &family) const {
    std::lock_guard lk(m_mutex);
    return m_data.find(family) != m_data.end();
  }

  // Returns up to `limit` series (insertion order) for a metric family.
  std::vector<Series> get_series(const std::string &family,
                                 std::size_t limit) const {
    std::vector<Series> out;
    std::lock_guard lk(m_mutex);
    const auto it = m_data.find(family);
    if (it == m_data.end()) {
      return out;
    }
    for (const auto &kv : it->second) {
      if (out.size() >= limit) {
        break;
      }
      Series s;
      s.labels = kv.first;
      s.points.assign(kv.second.begin(), kv.second.end());
      out.push_back(std::move(s));
    }
    return out;
  }

private:
  void run(std::stop_token st) {
    while (!st.stop_requested()) {
      sample();
      std::unique_lock lk(m_cv_mutex);
      m_cv.wait_for(lk, st, m_interval, [&] { return st.stop_requested(); });
    }
  }

  void sample() {
    if (!m_registry) {
      return;
    }
    const auto families = m_registry->Collect();
    const std::time_t now = std::time(nullptr);
    std::lock_guard lk(m_mutex);
    for (const auto &family : families) {
      if (family.metric.empty()) {
        continue;
      }
      auto &smap = m_data[family.name];
      std::size_t stored = 0;
      std::span<const prometheus::ClientMetric> view(family.metric);
      for (const auto &metric : view) {
        if (stored >= m_max_series_per_family) {
          break;
        }
        const std::string key = mh_format_labels(metric.label);
        const double val = extract_value(metric, family.type);
        auto &buf = smap[key];
        if (buf.empty() || buf.back().first != now) {
          buf.emplace_back(now, val);
          if (buf.size() > m_max_samples) {
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

  std::shared_ptr<prometheus::Registry> m_registry;
  std::chrono::seconds m_interval;
  std::size_t m_max_series_per_family;
  std::size_t m_max_samples;
  mutable std::mutex m_mutex;
  std::map<std::string,
           std::map<std::string,
                    std::deque<std::pair<std::time_t, double>>>>
      m_data;
  std::jthread m_thread;
  std::mutex m_cv_mutex;
  std::condition_variable_any m_cv;
};

#endif // METRICS_HISTORY_HPP
