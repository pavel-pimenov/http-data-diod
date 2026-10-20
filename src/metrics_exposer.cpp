#include "metrics_exposer.hpp"

#include "logger.hpp"

#include <prometheus/text_serializer.h>

#include <iterator>
#include <sstream>
#include <utility>

namespace {
constexpr const char *kMetricsContentType =
    "text/plain; version=0.0.4; charset=utf-8";
} // namespace

MetricsExposer::MetricsExposer(int port) : m_port(port) {
  m_server.Get("/metrics", [this](const httplib::Request & /*req*/,
                                 httplib::Response &res) {
    res.set_content(serialize(), kMetricsContentType);
  });
  if (m_port == 0) {
    // Bind here (not in the serving thread) so the caller learns the port
    // before the first scrape.
    m_port = m_server.bind_to_any_port("0.0.0.0");
  }
  m_thread = std::thread([this] { serve(); });
}

MetricsExposer::~MetricsExposer() {
  m_server.stop();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void MetricsExposer::serve() {
  Logger::info("Metrics exposer listening on 0.0.0.0:{}", m_port);
  m_server.listen("0.0.0.0", m_port);
}

void MetricsExposer::register_collectable(
    std::shared_ptr<prometheus::Collectable> collectable) {
  const std::scoped_lock lock(m_mutex);
  m_collectables.push_back(std::move(collectable));
}

std::string MetricsExposer::serialize() const {
  std::vector<prometheus::MetricFamily> metrics;
  {
    const std::scoped_lock lock(m_mutex);
    for (const auto &collectable : m_collectables) {
      auto collected = collectable->Collect();
      metrics.insert(metrics.end(), std::make_move_iterator(collected.begin()),
                     std::make_move_iterator(collected.end()));
    }
  }
  std::ostringstream out;
  prometheus::TextSerializer const serializer;
  serializer.Serialize(out, metrics);
  return out.str();
}
