#ifndef SCOPED_METRICS_HPP
#define SCOPED_METRICS_HPP

#include <prometheus/counter.h>

class ScopedMetrics {
public:
  explicit ScopedMetrics(prometheus::Counter &counter) : m_counter(counter) {}
  ~ScopedMetrics() { m_counter.Increment(); }

private:
  prometheus::Counter &m_counter;
};

#endif // SCOPED_METRICS_HPP