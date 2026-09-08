#include "sentry_client.hpp"

#include "random_utils.hpp"
#include "time_utils.hpp"
#include "logger.hpp"
#include "httplib/httplib.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace {

void append_hex_digits(uint64_t value, std::string &out) {
  static constexpr char k_hex_digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    out.push_back(k_hex_digits[(value >> (4 * i)) & 0xF]);
  }
}

std::string generate_event_id() {
  std::string hex_id;
  hex_id.reserve(32);
  for (size_t i = 0; i < 2; ++i) {
    append_hex_digits(RandomUtils::rng()(), hex_id);
  }
  return hex_id;
}

} // namespace

namespace sentry {

std::optional<DsnData> parse_dsn(std::string_view dsn) {
  if (dsn.empty()) {
    return std::nullopt;
  }

  std::string_view rest = dsn;
  std::string scheme = "https";
  const size_t scheme_sep = rest.find("://");
  if (scheme_sep != std::string_view::npos) {
    scheme = std::string(rest.substr(0, scheme_sep));
    if (scheme != "http" && scheme != "https") {
      return std::nullopt;
    }
    rest.remove_prefix(scheme_sep + 3);
  }

  const size_t at = rest.find('@');
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string auth(rest.substr(0, at));
  rest.remove_prefix(at + 1);

  if (auth.empty()) {
    return std::nullopt;
  }
  const size_t secret_sep = auth.find(':');
  const std::string public_key =
      auth.substr(0, secret_sep == std::string::npos ? std::string::npos
                                                     : secret_sep);
  if (public_key.empty()) {
    return std::nullopt;
  }
  const std::string secret_key =
      secret_sep == std::string::npos
          ? ""
          : auth.substr(secret_sep + 1);

  const size_t last_slash = rest.rfind('/');
  if (last_slash == std::string_view::npos ||
      last_slash == rest.size() - 1) {
    return std::nullopt;
  }
  const std::string project_id(rest.substr(last_slash + 1));
  if (project_id.empty()) {
    return std::nullopt;
  }
  const std::string host_and_path(rest.substr(0, last_slash));
  const size_t first_slash = host_and_path.find('/');
  const std::string host_port =
      host_and_path.substr(0, first_slash);
  std::string host = host_port;
  const size_t port_sep = host_port.rfind(':');
  if (port_sep != std::string::npos) {
    host = host_port.substr(0, port_sep);
  }
  if (host.empty() || host.find_first_of("[] \t") != std::string::npos) {
    return std::nullopt;
  }
  int port = scheme == "https" ? 443 : 80;
  if (port_sep != std::string::npos && port_sep + 1 < host_port.size()) {
    const std::string_view port_str =
        std::string_view(host_port).substr(port_sep + 1);
    if (port_str.empty() ||
        port_str.find_first_not_of("0123456789") != std::string_view::npos) {
      return std::nullopt;
    }
    int parsed_port = 0;
    for (const char digit : port_str) {
      parsed_port = parsed_port * 10 + (digit - '0');
    }
    if (parsed_port <= 0 || parsed_port > 65535) {
      return std::nullopt;
    }
    port = parsed_port;
  }
  std::string path_prefix = first_slash == std::string::npos
                                ? ""
                                : host_and_path.substr(first_slash);
  while (!path_prefix.empty() && path_prefix.back() == '/') {
    path_prefix.pop_back();
  }
  if (path_prefix == "/") {
    path_prefix.clear();
  }

  return DsnData{scheme, host, port, path_prefix, public_key, secret_key,
                 project_id};
}

std::string level_to_string(EventLevel level) {
  switch (level) {
  case EventLevel::Debug:
    return "debug";
  case EventLevel::Info:
    return "info";
  case EventLevel::Warning:
    return "warning";
  case EventLevel::Error:
    return "error";
  case EventLevel::Fatal:
    return "fatal";
  }
  return "error";
}

nlohmann::json build_event_json(const SentryEvent &event,
                                const std::string &service_name,
                                const std::string &environment,
                                const std::string &release) {
  nlohmann::json root = nlohmann::json::object();
  root["event_id"] = generate_event_id();
  root["timestamp"] = TimeUtils::format_rfc3339();
  root["platform"] = "native";
  root["level"] = level_to_string(event.m_level);
  root["message"] = event.m_message;

  if (!release.empty()) {
    root["release"] = release;
  }
  if (!environment.empty()) {
    root["environment"] = environment;
  }
  if (!event.m_transaction.empty()) {
    root["transaction"] = event.m_transaction;
  }

  nlohmann::json tags = nlohmann::json::object();
  if (!service_name.empty()) {
    tags["service"] = service_name;
  }
  if (!event.m_request_id.empty()) {
    tags["request_id"] = event.m_request_id;
  }
  for (const auto &entry : event.m_tags.items()) {
    if (entry.value().is_string()) {
      tags[entry.key()] = entry.value();
    } else {
      tags[entry.key()] = entry.value().dump();
    }
  }
  if (!tags.empty()) {
    root["tags"] = tags;
  }

  if (!event.m_fingerprint.empty()) {
    root["fingerprint"] = event.m_fingerprint;
  }
  if (!event.m_extra.empty()) {
    root["extra"] = event.m_extra;
  }

  if (event.m_level == EventLevel::Error || event.m_level == EventLevel::Fatal) {
    root["exception"] = nlohmann::json{{"values",
                                        nlohmann::json::array(
                                            {{{"type", "Error"},
                                              {"value", event.m_message}}})}};
  }

  return root;
}

std::string build_envelope(const SentryEvent &event, const DsnData &dsn,
                           const std::string &service_name,
                           const std::string &environment,
                           const std::string &release) {
  const nlohmann::json event_json =
      build_event_json(event, service_name, environment, release);
  const std::string event_id = event_json.value("event_id", "");

  const nlohmann::json header = {
      {"event_id", event_id},
      {"sent_at", TimeUtils::format_rfc3339()},
      {"sdk", {{"name", "http-data-diod"}, {"version", "0"}}}};

  std::string sent_key = dsn.m_public_key;
  if (!dsn.m_secret_key.empty()) {
    sent_key += ":" + dsn.m_secret_key;
  }
  const nlohmann::json auth = {
      {"sent_key", sent_key}, {"sent_version", "7"}};
  const nlohmann::json item_header = {{"type", "event"}};

  return header.dump() + "\n" + auth.dump() + "\n" + item_header.dump() + "\n" +
         event_json.dump();
}

} // namespace sentry

SentryClient::SentryClient(std::string dsn, prometheus::Counter &events_sent,
                           prometheus::Counter &events_failed,
                           prometheus::Gauge &queue_size,
                           std::string service_name, std::string environment,
                           std::string release, int timeout_ms,
                           size_t max_queue_size, const TransportFn &transport)
    : m_dsn(std::move(dsn)),
      m_dsn_data(sentry::parse_dsn(m_dsn)),
      m_service_name(std::move(service_name)),
      m_environment(std::move(environment)),
      m_release(std::move(release)),
      m_events_sent(events_sent),
      m_events_failed(events_failed),
      m_queue_size(queue_size),
      m_timeout_ms(timeout_ms),
      m_max_queue_size(max_queue_size == 0 ? 1 : max_queue_size),
      m_transport(transport) {
  if (enabled()) {
    Logger::info("Sentry client enabled: host={} project={} service={}",
                 m_dsn_data->m_host, m_dsn_data->m_project_id,
                 m_service_name);
    m_sender_thread = std::jthread([this](std::stop_token st) {
      sender_loop(std::move(st));
    });
  }
}

SentryClient::~SentryClient() {
  if (m_sender_thread.joinable()) {
    m_sender_thread.request_stop();
    m_cv.notify_all();
    m_sender_thread.join();
  }
}

bool SentryClient::enabled() const { return m_dsn_data.has_value(); }

void SentryClient::capture(const sentry::SentryEvent &event) {
  if (!enabled()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() >= m_max_queue_size) {
      m_queue.pop_front();
      --m_pending;
      m_events_failed.Increment();
    }
    m_queue.push_back(event);
    ++m_pending;
    m_queue_size.Set(static_cast<double>(m_pending));
  }
  m_cv.notify_one();
}

void SentryClient::capture_message(
    const std::string &message, const std::string &request_id,
    const std::vector<std::string> &fingerprint) {
  sentry::SentryEvent event;
  event.m_message = message;
  event.m_request_id = request_id;
  event.m_fingerprint = fingerprint;
  capture(event);
}

void SentryClient::flush() {
  if (!enabled()) {
    return;
  }
  std::unique_lock<std::mutex> lock(m_mutex);
  m_cv.wait(lock, [this] { return m_pending.load() == 0; });
}

// NOLINTNEXTLINE(performance-unnecessary-value-param) - cv.wait requires stop_token by value
void SentryClient::sender_loop(std::stop_token st) {
  while (true) {
    std::vector<sentry::SentryEvent> batch;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, st, [this] { return !m_queue.empty(); });
      if (m_queue.empty()) {
        break;
      }
      const size_t to_take = std::min(m_queue.size(), size_t{8});
      batch.reserve(to_take);
      for (size_t i = 0; i < to_take; ++i) {
        batch.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
      }
    }
    for (const auto &event : batch) {
      process_event(event);
    }
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_pending -= batch.size();
      m_queue_size.Set(static_cast<double>(m_pending));
      m_cv.notify_all();
    }
  }
  Logger::info("{}", "Sentry sender thread stopped");
}

void SentryClient::process_event(const sentry::SentryEvent &event) {
  if (!m_dsn_data) {
    return;
  }
  const std::string envelope = sentry::build_envelope(
      event, *m_dsn_data, m_service_name, m_environment, m_release);
  bool delivered = false;
  if (m_transport) {
    delivered = m_transport(envelope);
  } else {
    delivered = send_envelope(envelope);
  }
  if (delivered) {
    m_events_sent.Increment();
  } else {
    m_events_failed.Increment();
  }
}

bool SentryClient::send_envelope(const std::string &envelope) {
  if (!m_dsn_data) {
    return false;
  }
  const std::string path =
      m_dsn_data->m_path_prefix + "/api/" + m_dsn_data->m_project_id +
      "/envelope/";
  const int timeout_seconds = m_timeout_ms / 1000;

  httplib::Result res;
  if (m_dsn_data->m_scheme == "https") {
    httplib::SSLClient client(m_dsn_data->m_host, m_dsn_data->m_port);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(timeout_seconds, 0);
    client.set_write_timeout(timeout_seconds, 0);
    res = client.Post(path, httplib::Headers{}, envelope,
                      "application/x-sentry-envelope");
  } else {
    httplib::Client client(m_dsn_data->m_host, m_dsn_data->m_port);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(timeout_seconds, 0);
    client.set_write_timeout(timeout_seconds, 0);
    res = client.Post(path, httplib::Headers{}, envelope,
                      "application/x-sentry-envelope");
  }
  return res && res->status >= 200 && res->status < 300;
}