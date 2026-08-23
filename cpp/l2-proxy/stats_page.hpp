#ifndef STATS_PAGE_HPP
#define STATS_PAGE_HPP

#include <chrono>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>
#include <prometheus/registry.h>

// Builds a self-contained HTML status page from a Prometheus registry so service
// health can be assessed without Grafana. Pulls current gauge/counter values via
// Registry::Collect(); histograms/summaries are summarized by count+sum. The page
// is offline-friendly (inline CSS, no external assets) and auto-refreshes.

namespace {

inline std::string escape_html(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

inline std::string format_metric_value(const prometheus::ClientMetric &m,
                                      prometheus::MetricType type) {
  std::ostringstream os;
  switch (type) {
    case prometheus::MetricType::Counter:
    case prometheus::MetricType::Gauge:
      os << m.gauge.value;
      break;
    case prometheus::MetricType::Histogram:
      os << "count=" << m.histogram.sample_count
         << " sum=" << m.histogram.sample_sum;
      break;
    case prometheus::MetricType::Summary:
      os << "count=" << m.summary.sample_count
         << " sum=" << m.summary.sample_sum;
      break;
    default:
      os << m.gauge.value;
      break;
  }
  return os.str();
}

inline std::string format_labels(const std::vector<prometheus::ClientMetric::Label> &label) {
  if (label.empty()) {
    return "";
  }
  std::ostringstream os;
  os << "{";
  for (size_t i = 0; i < label.size(); ++i) {
    if (i > 0) {
      os << ", ";
    }
    os << label[i].name << "=" << label[i].value;
  }
  os << "}";
  return os.str();
}

} // namespace

inline std::string build_stats_html(
    const std::string &service_name,
    const std::shared_ptr<prometheus::Registry> &registry) {
  const auto families = registry->Collect();

  // Derive an overall readiness banner from the *_health_ready gauges and the
  // *_nats_connected gauge when present.
  bool has_health = false;
  bool ready = true;
  bool nats_ok = true;
  bool has_nats = false;
  for (const auto &family : families) {
    for (const auto &metric : family.metric) {
      if (family.name.find("health_ready") != std::string::npos) {
        has_health = true;
        if (metric.gauge.value != 1.0) {
          ready = false;
        }
      }
      if (family.name.find("nats_connected") != std::string::npos) {
        has_nats = true;
        if (metric.gauge.value != 1.0) {
          nats_ok = false;
        }
      }
    }
  }
  const bool overall_ok = (!has_health || ready) && (!has_nats || nats_ok);
  const char *banner_color = overall_ok ? "#1f7a3d" : "#a3271f";
  const char *banner_text =
      overall_ok ? "OPERATIONAL" : "DEGRADED";

  std::ostringstream html;
  html << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
  html << "<meta charset=\"utf-8\">\n";
  html << "<meta http-equiv=\"refresh\" content=\"5\">\n";
  html << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  html << "<title>" << escape_html(service_name) << " — status</title>\n";
  html << "<style>\n";
  html << "body{font-family:ui-monospace,Menlo,Consolas,monospace;"
          "margin:0;background:#0d1117;color:#c9d1d9;}\n";
  html << ".banner{padding:18px 24px;font-size:20px;font-weight:700;"
          "letter-spacing:1px;color:#fff;background:"
       << banner_color << ";}\n";
  html << ".wrap{max-width:1100px;margin:0 auto;padding:20px;}\n";
  html << "h1{font-size:18px;margin:0 0 4px;}\n";
  html << ".ts{color:#8b949e;font-size:12px;margin-bottom:18px;}\n";
  html << ".family{margin:0 0 18px;border:1px solid #21262d;border-radius:8px;"
          "overflow:hidden;}\n";
  html << ".fhead{background:#161b22;padding:8px 14px;font-size:14px;"
          "font-weight:600;}\n";
  html << ".fhelp{color:#8b949e;font-size:11px;font-weight:400;margin-top:2px;}\n";
  html << "table{width:100%;border-collapse:collapse;font-size:13px;}\n";
  html << "td{padding:5px 14px;border-top:1px solid #21262d;}\n";
  html << ".val{text-align:right;color:#79c0ff;white-space:nowrap;}\n";
  html << ".labels{color:#d2a8ff;}\n";
  html << "tr:hover td{background:#1c2128;}\n";
  html << "</style>\n</head>\n<body>\n";
  html << "<div class=\"banner\">" << banner_text << " — "
       << escape_html(service_name) << "</div>\n";
  html << "<div class=\"wrap\">\n";
  html << "<h1>" << escape_html(service_name) << " metrics</h1>\n";

  const std::time_t now = std::time(nullptr);
  char tsbuf[64];
  std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S UTC",
                std::gmtime(&now));
  html << "<div class=\"ts\">generated " << tsbuf
       << " · auto-refresh 5s · source: Prometheus registry</div>\n";

  for (const auto &family : families) {
    if (family.metric.empty()) {
      continue;
    }
    html << "<div class=\"family\">\n";
    html << "<div class=\"fhead\">" << escape_html(family.name);
    if (!family.help.empty()) {
      html << "<div class=\"fhelp\">" << escape_html(family.help) << "</div>";
    }
    html << "</div>\n";
    html << "<table>\n";
    for (const auto &metric : family.metric) {
      const std::string labels = format_labels(metric.label);
      html << "<tr><td class=\"labels\">" << escape_html(labels) << "</td><td class=\"val\">"
           << escape_html(format_metric_value(metric, family.type)) << "</td></tr>\n";
    }
    html << "</table>\n</div>\n";
  }

  html << "</div>\n</body>\n</html>\n";
  return html.str();
}

#endif // STATS_PAGE_HPP
