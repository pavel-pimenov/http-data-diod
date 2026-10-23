#ifndef METRICS_EXPOSER_HPP
#define METRICS_EXPOSER_HPP

#include "httplib/httplib.h"

#include <prometheus/collectable.h>
#include <prometheus/histogram.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A minimal /metrics endpoint built on httplib (already vendored for the
// proxy's own HTTP server). It replaces prometheus-cpp's pull module.
// The exposition text itself is produced by prometheus-cpp's own TextSerializer
// (the core module, already linked), so the output stays byte-identical to the
// previous implementation.
class MetricsExposer {
public:
  // A zero port binds an ephemeral one (the bound port is then available via
  // port()); the services pass their fixed metrics port.
  //
  // render_duration (optional) measures how long the exposition takes: the
  // scrape cost grows with the cardinality, so it has to be visible.
  explicit MetricsExposer(int port,
                          prometheus::Histogram *render_duration = nullptr);
  ~MetricsExposer();

  MetricsExposer(const MetricsExposer &) = delete;
  MetricsExposer &operator=(const MetricsExposer &) = delete;

  // Any collectable: the mode's registry, the shared common registry, or the
  // dynamic per-client-id families (all of them are Collectables).
  void register_collectable(std::shared_ptr<prometheus::Collectable> collectable);

  [[nodiscard]] int port() const { return m_port; }

private:
  void serve();
  [[nodiscard]] std::string serialize() const;

  httplib::Server m_server;
  int m_port;
  prometheus::Histogram *m_render_duration = nullptr;
  std::vector<std::shared_ptr<prometheus::Collectable>> m_collectables;
  mutable std::mutex m_mutex;
  std::thread m_thread;
};

#endif // METRICS_EXPOSER_HPP
