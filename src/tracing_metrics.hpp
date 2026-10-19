#ifndef TRACING_METRICS_HPP
#define TRACING_METRICS_HPP

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

// Prometheus handles of the Jaeger delivery path. The AppContext registers them
// on the shared common registry and the JaegerLogger writes them, so both
// sides speak one struct instead of the logger taking six separate references
// that every construction site had to wire by hand.
//
// The handles are owned by the registry and injected at construction; the
// Sentry/GlitchTip transaction counters are not part of this struct because the
// logger treats them as optional (they are only written when a Sentry DSN is
// configured) and keeps them as pointers.
struct JaegerDeliveryMetrics {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -
  // Intentional design for Prometheus metrics
  prometheus::Counter &m_spans_sent;
  prometheus::Counter &m_spans_failed;
  prometheus::Gauge &m_queue_size;
  prometheus::Gauge &m_last_send_duration;
  prometheus::Histogram &m_send_latency;
  prometheus::Histogram &m_queue_time;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

#endif // TRACING_METRICS_HPP
