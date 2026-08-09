#ifndef SCOPED_PROFILER_HPP
#define SCOPED_PROFILER_HPP

#include "labeled_histogram_collector.hpp"
#include <chrono>
#include <prometheus/histogram.h>
#include <string>

class ScopedProfiler {
public:
  explicit ScopedProfiler(prometheus::Histogram &histogram)
      : m_histogram(histogram), m_start_time(std::chrono::steady_clock::now()) {
  }

  ~ScopedProfiler() {
    const auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> duration = end_time - m_start_time;
    m_histogram.Observe(duration.count());
  }

private:
  prometheus::Histogram &m_histogram;
  std::chrono::steady_clock::time_point m_start_time;
};

// RAII latency observer for a LabeledHistogramCollector: records the elapsed
// time under a specific label value (e.g. X-DataHub-Client-Id). A null
// collector pointer makes it a no-op, so it is safe to instantiate before the
// per-client metrics are wired up.
class ScopedLabeledProfiler {
public:
  explicit ScopedLabeledProfiler(LabeledHistogramCollector *collector,
                                 const std::string &label_value)
      : m_collector(collector), m_label_value(label_value),
        m_start_time(std::chrono::steady_clock::now()) {}

  ~ScopedLabeledProfiler() {
    if (m_collector != nullptr) {
      const auto end_time = std::chrono::steady_clock::now();
      std::chrono::duration<double> duration = end_time - m_start_time;
      m_collector->observe(m_label_value, duration.count());
    }
  }

private:
  LabeledHistogramCollector *m_collector;
  const std::string m_label_value;
  std::chrono::steady_clock::time_point m_start_time;
};

#endif // SCOPED_PROFILER_HPP
