#ifndef STATS_PAGE_HPP
#define STATS_PAGE_HPP

#include <chrono>
#include <ctime>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#if __has_include(<execution>)
#include <execution>
#endif

#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>
#include <prometheus/registry.h>

#include "metrics_history.hpp"

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

// Renders a tiny inline-SVG sparkline (Grafana-like "activity") for the last
// `window_minutes` of samples. For rate metrics the polyline shows the
// per-second delta between consecutive samples; for gauges the raw value.
// Returns empty string if there are not enough samples to draw a line.
inline std::string build_sparkline_svg(
    const std::vector<std::pair<std::time_t, double>> &raw_pts, bool as_rate,
    int window_minutes, int width = 188, int height = 26) {
  const std::time_t cutoff =
      (raw_pts.empty() ? 0 : raw_pts.back().first) - window_minutes * 60;
  std::vector<std::pair<std::time_t, double>> pts;
  for (const auto &p : raw_pts) {
    if (p.first >= cutoff) {
      pts.push_back(p);
    }
  }
  if (pts.size() < 2) {
    return "";
  }
  std::vector<double> vals;
  vals.reserve(pts.size());
  if (as_rate) {
    for (size_t i = 1; i < pts.size(); ++i) {
      const double dt = static_cast<double>(pts[i].first - pts[i - 1].first);
      double r = (pts[i].second - pts[i - 1].second) / (dt > 0.0 ? dt : 1.0);
      if (r < 0.0) {
        r = 0.0; // counter reset between samples
      }
      vals.push_back(r);
    }
  } else {
    for (const auto &p : pts) {
      vals.push_back(p.second);
    }
  }
  double min_v = vals[0];
  double max_v = vals[0];
  for (const double v : vals) {
    if (v < min_v) {
      min_v = v;
    }
    if (v > max_v) {
      max_v = v;
    }
  }
  const double range = (max_v - min_v) > 1e-12 ? (max_v - min_v) : 1.0;
  const double w = static_cast<double>(width);
  const double h = static_cast<double>(height);
  std::ostringstream pts_attr;
  for (size_t i = 0; i < vals.size(); ++i) {
    const double x = vals.size() == 1
                         ? 0.0
                         : (static_cast<double>(i) / (vals.size() - 1)) * w;
    const double y = h - ((vals[i] - min_v) / range) * (h - 2.0) - 1.0;
    pts_attr << (i ? " " : "") << x << "," << y;
  }
  std::ostringstream svg;
  svg << "<svg class=\"spark\" viewBox=\"0 0 " << width << " " << height
      << "\" preserveAspectRatio=\"none\" width=\"100%\" height=\"" << height
      << "\"><polyline fill=\"none\" stroke=\"#79c0ff\" stroke-width=\"1.5\" points=\""
      << pts_attr.str() << "\"/></svg>";
  return svg.str();
}

inline std::string build_stats_html(
    const std::string &service_name,
    const std::shared_ptr<prometheus::Registry> &registry,
    const MetricsHistory *history = nullptr, int window_minutes = 30) {
  const auto families = registry->Collect();
  std::span<const prometheus::MetricFamily> fam_view(families);
#if __has_include(<ranges>) && defined(__cpp_lib_ranges_chunk)
  auto fam_chunks = fam_view | std::views::chunk(4); (void)fam_chunks; // chunk demo: плитка 4×N
#endif

  // Derive an overall readiness banner from the *_health_ready gauges and the
  // *_nats_connected gauge when present.
  bool has_health = false;
  bool ready = true;
  bool nats_ok = true;
  bool has_nats = false;
  for (const auto &family : fam_view) {
    std::span<const prometheus::ClientMetric> mview(family.metric);
    for (const auto &metric : mview) {
      if (family.name.find("health_ready") != std::string::npos) {
        has_health = true;
        if (metric.gauge.value != 1.0) ready = false;
      }
      if (family.name.find("nats_connected") != std::string::npos) {
        has_nats = true;
        if (metric.gauge.value != 1.0) nats_ok = false;
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
           "padding:8px 10px;overflow:hidden;max-height:192px;"
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
   html << ".sparkwrap{margin-top:5px;border-top:1px solid #21262d;padding-top:4px;}\n";
   html << ".sparkwrap .cap{font-size:9px;color:#8b949e;}\n";
   html << ".spark{display:block;width:100%;}\n";
   html << "</style>\n</head>\n<body>\n";
  html << "<div class=\"banner\"><span>" << banner_text << " — "
       << escape_html(service_name) << "</span><span class=\"ts\">" << tsbuf
       << " · 5s</span></div>\n";
  html << "<div class=\"grid\">\n";

  // Cap series per tile so labeled/dense families (per-IP, per-client-id) don't
  // blow up the card height and break the no-scroll layout.
  constexpr std::size_t kMaxSeriesPerTile = 6;

  for (const auto &family : fam_view) {
    if (family.metric.empty()) {
      continue;
    }
    html << "<div class=\"tile\">\n";
    html << "<div class=\"tname\">" << escape_html(family.name) << "</div>\n";
    if (!family.help.empty()) {
      html << "<div class=\"thelp\">" << escape_html(family.help) << "</div>\n";
    }
    html << "<div class=\"vals\">\n";
    std::span<const prometheus::ClientMetric> mview(family.metric);
    const std::size_t shown = std::min<std::size_t>(mview.size(), kMaxSeriesPerTile);
    // ranges::views::take — C++23 сахар вместо ручного min+for
    for (const auto &metric : mview | std::views::take(shown)) {
      const std::string labels = format_labels(metric.label);
      html << "<div class=\"vrow\"><span class=\"labels\">" << escape_html(labels)
           << "</span> <span class=\"val\">"
           << escape_html(format_metric_value(metric, family.type))
           << "</span></div>\n";
    }
    if (mview.size() > shown) {
      html << "<div class=\"vrow more\">+" << (mview.size() - shown) << " more</div>\n";
    }
    html << "</div>\n";

    // One compact sparkline per tile: the family's representative series
    // (unlabeled/total if present, else the most active labeled series).
    if (history && history->has_family(family.name)) {
      const bool as_rate =
          (family.type == prometheus::MetricType::Counter ||
           family.type == prometheus::MetricType::Histogram ||
           family.type == prometheus::MetricType::Summary);
      const auto series = history->get_series(family.name, 16);
      const MetricsHistory::Series *repr = nullptr;
      double best = -1.0;
      for (const auto &s : series) {
        if (s.labels.empty()) {
          repr = &s;
          break;
        }
        const double last = s.points.empty() ? 0.0 : s.points.back().second;
        if (last > best) {
          best = last;
          repr = &s;
        }
      }
      if (repr) {
        const std::string svg =
            build_sparkline_svg(repr->points, as_rate, window_minutes);
        if (!svg.empty()) {
          html << "<div class=\"sparkwrap\"><div class=\"cap\">last "
               << window_minutes << "m</div>" << svg << "</div>\n";
        }
      }
    }

    html << "</div>\n";
  }

  html << "</div>\n</body>\n</html>\n";
  return html.str();
}

#endif // STATS_PAGE_HPP
