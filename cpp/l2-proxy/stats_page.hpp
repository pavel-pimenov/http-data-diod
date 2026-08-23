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
  const std::time_t now = std::time(nullptr);
  char tsbuf[64];
  std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S UTC",
                std::gmtime(&now));

  html << "<style>\n";
  html << "html,body{height:100%;}\n";
  html << "body{font-family:ui-monospace,Menlo,Consolas,monospace;margin:0;"
          "background:#0d1117;color:#c9d1d9;display:flex;flex-direction:column;"
          "height:100vh;overflow:hidden;}\n";
  html << ".banner{padding:10px 18px;font-size:17px;font-weight:700;"
          "letter-spacing:1px;color:#fff;background:"
       << banner_color
       << ";flex:0 0 auto;display:flex;justify-content:space-between;"
          "align-items:center;}\n";
  html << ".banner .ts{font-size:11px;font-weight:400;opacity:.85;}\n";
  // Tile grid: fills the viewport, no vertical scroll. Each metric family is a
  // compact card; dense/labeled families are capped so the grid stays bounded.
  html << ".grid{flex:1 1 auto;overflow:hidden;padding:12px;display:grid;gap:10px;"
          "grid-template-columns:repeat(auto-fill,minmax(190px,1fr));"
          "align-content:start;}\n";
  html << ".tile{border:1px solid #21262d;border-radius:8px;background:#161b22;"
          "padding:8px 10px;overflow:hidden;max-height:132px;"
          "display:flex;flex-direction:column;}\n";
  html << ".tname{font-size:12px;font-weight:600;color:#e6edf3;white-space:nowrap;"
          "overflow:hidden;text-overflow:ellipsis;}\n";
  html << ".thelp{font-size:10px;color:#8b949e;white-space:nowrap;overflow:hidden;"
          "text-overflow:ellipsis;margin-top:1px;}\n";
  html << ".vals{font-size:11px;margin-top:4px;overflow:hidden;}\n";
  html << ".vrow{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}\n";
  html << ".vrow .labels{color:#d2a8ff;}\n";
  html << ".vrow .val{color:#79c0ff;}\n";
  html << ".more{color:#8b949e;font-style:italic;}\n";
  html << "</style>\n</head>\n<body>\n";
  html << "<div class=\"banner\"><span>" << banner_text << " — "
       << escape_html(service_name) << "</span><span class=\"ts\">" << tsbuf
       << " · 5s</span></div>\n";
  html << "<div class=\"grid\">\n";

  // Cap series per tile so labeled/dense families (per-IP, per-client-id) don't
  // blow up the card height and break the no-scroll layout.
  constexpr std::size_t kMaxSeriesPerTile = 6;

  for (const auto &family : families) {
    if (family.metric.empty()) {
      continue;
    }
    html << "<div class=\"tile\">\n";
    html << "<div class=\"tname\">" << escape_html(family.name) << "</div>\n";
    if (!family.help.empty()) {
      html << "<div class=\"thelp\">" << escape_html(family.help) << "</div>\n";
    }
    html << "<div class=\"vals\">\n";
    const std::size_t shown =
        std::min<std::size_t>(family.metric.size(), kMaxSeriesPerTile);
    for (std::size_t i = 0; i < shown; ++i) {
      const auto &metric = family.metric[i];
      const std::string labels = format_labels(metric.label);
      html << "<div class=\"vrow\"><span class=\"labels\">" << escape_html(labels)
           << "</span> <span class=\"val\">"
           << escape_html(format_metric_value(metric, family.type))
           << "</span></div>\n";
    }
    if (family.metric.size() > shown) {
      html << "<div class=\"vrow more\">+"
           << (family.metric.size() - shown) << " more</div>\n";
    }
    html << "</div>\n</div>\n";
  }

  html << "</div>\n</body>\n</html>\n";
  return html.str();
}

#endif // STATS_PAGE_HPP
