#include "sentry_client.hpp"

#include "async_sender_queue.hpp"
#include "http_setup.hpp"
#include "json_utils.hpp"
#include "nlohmann/json.hpp"
#include "prometheus/counter.h"
#include "prometheus/gauge.h"
#include "random_utils.hpp"
#include "time_utils.hpp"
#include "logger.hpp"
#include "httplib/httplib.h"
#include <cstdint>
#include <string>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void append_hex_digits(uint64_t value, std::string &out) {
  static constexpr std::string_view k_hex_digits = "0123456789abcdef";
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
  // The check above rejects a trailing slash, so at least one character always
  // follows it and the project id is never empty.
  const std::string project_id(rest.substr(last_slash + 1));
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
  if (port_sep != std::string::npos && port_sep + 1 < host_port.size()) { //-V1051
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
  // Stripping trailing slashes also turns a bare "/" into "", so no separate
  // "/"-case is needed here.
  while (!path_prefix.empty() && path_prefix.back() == '/') {
    path_prefix.pop_back();
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

std::string envelope_path(const DsnData &dsn) {
  return dsn.m_path_prefix + "/api/" + dsn.m_project_id + "/envelope/";
}

std::string auth_header(const DsnData &dsn) {
  std::string key = dsn.m_public_key;
  if (!dsn.m_secret_key.empty()) {
    key += "/" + dsn.m_secret_key;
  }
  return "Sentry sentry_version=7, sentry_key=" + key;
}

nlohmann::json envelope_header(const std::string &event_id) {
  return nlohmann::json{
      {"event_id", event_id},
      {"sent_at", TimeUtils::format_rfc3339()},
      {"sdk", {{"name", "http-data-diod"}, {"version", "0"}}}};
}

std::string envelope_item(const nlohmann::json &payload, std::string_view type,
                          bool include_length) {
  const std::string body = payload.dump();
  nlohmann::json item = {{"type", std::string(type)}};
  if (include_length) {
    item["length"] = body.size();
  }
  return item.dump() + "\n" + body;
}

nlohmann::json service_tags(const std::string &service_name) {
  nlohmann::json tags = nlohmann::json::object();
  if (!service_name.empty()) {
    tags["service"] = service_name;
  }
  return tags;
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

  nlohmann::json tags = service_tags(service_name);
  if (!event.m_request_id.empty()) {
    tags["request_id"] = event.m_request_id;
  }
  merge_stringified_tags(tags, event.m_tags);
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
  (void)dsn;
  const nlohmann::json event_json =
      build_event_json(event, service_name, environment, release);
  const std::string event_id = event_json.value("event_id", "");

  // Modern Sentry-compatible servers (glitchtip >= 6) authenticate via the
  // X-Sentry-Auth HTTP header (added in send_envelope) and their Rust
  // envelope parser rejects the legacy in-body auth line, so it is omitted.
  return envelope_header(event_id).dump() + "\n" +
         envelope_item(event_json, "event");
}

} // namespace sentry

SentryClient::SentryClient(std::string dsn, prometheus::Counter &events_sent,
                           prometheus::Counter &events_failed,
                           prometheus::Gauge &queue_size,
                           std::string service_name, std::string environment,
                           std::string release, int timeout_ms,
                           size_t max_queue_size, const TransportFn &transport)
    : m_config{std::move(dsn),
               sentry::parse_dsn(m_config.m_dsn),
               std::move(service_name),
               std::move(environment),
               std::move(release),
               timeout_ms,
               max_queue_size == 0 ? 1 : max_queue_size,
               transport},
      m_metrics{events_sent, events_failed, queue_size},
      m_queue{m_config.m_max_queue_size,
              AsyncSenderQueue<sentry::SentryEvent>::OverflowPolicy::
                  DropOldest} {
  if (enabled()) {
    Logger::info("Sentry client enabled: host={} project={} service={}",
                 m_config.m_dsn_data->m_host,
                 m_config.m_dsn_data->m_project_id,
                 m_config.m_service_name);
    m_sender_thread = std::jthread([this](const std::stop_token &st) {
      sender_loop(st);
    });
  }
}

SentryClient::~SentryClient() {
  if (m_sender_thread.joinable()) {
    m_sender_thread.request_stop();
    m_queue.notify_all();
    m_sender_thread.join();
  }
}

bool SentryClient::enabled() const { return m_config.m_dsn_data.has_value(); }

void SentryClient::capture(const sentry::SentryEvent &event) {
  if (!enabled()) {
    return;
  }
  const auto outcome = m_queue.push(event);
  if (outcome.m_dropped) {
    m_metrics.m_events_failed.Increment();
  }
  m_metrics.m_queue_size.Set(static_cast<double>(outcome.m_size));
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
  m_queue.flush();
}

void SentryClient::sender_loop(const std::stop_token &st) {
  while (true) {
    std::vector<sentry::SentryEvent> batch;
    const auto result = m_queue.wait_and_take(st, 0, 8, true, batch);
    if (result ==
        AsyncSenderQueue<sentry::SentryEvent>::TakeResult::Stopped) {
      break;
    }
    for (const auto &event : batch) {
      process_event(event);
    }
    m_metrics.m_queue_size.Set(static_cast<double>(m_queue.size()));
    m_queue.mark_processed(batch.size());
  }
  Logger::info("{}", "Sentry sender thread stopped");
}

void SentryClient::process_event(const sentry::SentryEvent &event) {
  if (!m_config.m_dsn_data) {
    return;
  }
  const std::string envelope = sentry::build_envelope(
      event, *m_config.m_dsn_data, m_config.m_service_name,
      m_config.m_environment, m_config.m_release);
  bool delivered = false;
  if (m_config.m_transport) {
    delivered = m_config.m_transport(envelope);
  } else {
    delivered = send_envelope(envelope);
  }
  if (delivered) {
    m_metrics.m_events_sent.Increment();
  } else {
    m_metrics.m_events_failed.Increment();
  }
}

bool SentryClient::send_envelope(const std::string &envelope) {
  if (!m_config.m_dsn_data) {
    return false;
  }
  const std::string path = sentry::envelope_path(*m_config.m_dsn_data);
  // Modern Sentry-compatible servers (glitchtip >= 6) authenticate the DSN
  // via the X-Sentry-Auth header, not the URL userinfo part. The public key
  // is required; the legacy secret key goes after the slash if present.
  httplib::Headers const headers{
      {"X-Sentry-Auth", sentry::auth_header(*m_config.m_dsn_data)}};
  const int timeout_seconds = m_config.m_timeout_ms / 1000;

  httplib::Result res;
  if (m_config.m_dsn_data->m_scheme == "https") {
    httplib::SSLClient client(m_config.m_dsn_data->m_host,
                              m_config.m_dsn_data->m_port);
    setup_http_connection(client, timeout_seconds, false);
    res = client.Post(path, headers, envelope,
                      "application/x-sentry-envelope");
  } else {
    httplib::Client client(m_config.m_dsn_data->m_host,
                           m_config.m_dsn_data->m_port);
    setup_http_connection(client, timeout_seconds, false);
    res = client.Post(path, headers, envelope,
                      "application/x-sentry-envelope");
  }
  return res && res->status >= 200 && res->status < 300;
}