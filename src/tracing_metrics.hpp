#ifndef TRACING_METRICS_HPP
#define TRACING_METRICS_HPP

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

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
  // Spans lost on the way to Jaeger, split by reason. The untyped
  // m_spans_failed mixes "the exporter is down" with "the queue overflowed",
  // so an operator cannot tell a dead target from a slow one.
  prometheus::Family<prometheus::Counter> &m_spans_dropped;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

// Why a span was lost on the way to the exporter. The list is the single
// source of truth: the AppContext pre-creates a series per reason and the
// logger increments the matching one.
inline constexpr std::array<const char *, 4> g_tracing_drop_reasons = {
    "queue_full", "breaker_open", "export_error", "shutdown_flush"};

inline void count_span_drop(prometheus::Family<prometheus::Counter> &family,
                             std::string_view reason, std::size_t count) {
  family.Add({{"reason", std::string(reason)}}).Increment(count);
}

#endif // TRACING_METRICS_HPP
