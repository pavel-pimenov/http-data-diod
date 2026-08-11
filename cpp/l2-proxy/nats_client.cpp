#include "nats_client.hpp"
#include "json_utils.hpp"
#include <chrono>
#include <cstring>
#include <format>
#include <stdexcept>
#include <thread>

// Include NATS C headers
#include <nats/nats.h>

namespace {
// Converts a NATS status to a loggable string, guarding against null text.
std::string nats_status_text(natsStatus status) {
  const char *text = natsStatus_GetText(status);
  return text ? text : "unknown NATS error";
}

// Delivery callback shared by Subscribe/QueueSubscribe: forwards the message
// to the stored std::function callback and destroys the message. The closure is
// a pointer to the shared_ptr stored in NatsSubscription; a local copy is taken
// so the callback object stays alive for the duration of this call even if the
// subscription is being torn down concurrently.
void nats_message_callback(natsConnection *nc, natsSubscription *sub,
                           natsMsg *msg, void *closure) {
  (void)nc;
  (void)sub;
  auto *callback_holder =
      static_cast<std::shared_ptr<NatsMessageCallback> *>(closure);
  if (callback_holder) {
    const NatsMessageCallback callback = **callback_holder;
    if (callback && msg) {
      const char *data = natsMsg_GetData(msg);
      const int len = natsMsg_GetDataLength(msg);
      const char *subject = natsMsg_GetSubject(msg);
      const char *reply = natsMsg_GetReply(msg);
      callback(subject ? subject : "", std::string(data, len),
               reply ? reply : "");
    }
  }
  natsMsg_Destroy(msg);
}
} // namespace

NatsClient::NatsClient(const NatsConfig &cfg)
    : m_host(cfg.m_host), m_port(cfg.m_port), m_subject(cfg.m_subject),
      m_queue_group(cfg.m_queue_group), m_timeout_ms(cfg.m_timeout_ms),
      m_username(cfg.m_username), m_password(cfg.m_password),
      m_token(cfg.m_token), m_credentials_file(cfg.m_credentials_file),
      m_enable_tls(cfg.m_enable_tls), m_tls_cert_file(cfg.m_tls_cert_file),
      m_tls_key_file(cfg.m_tls_key_file),
      m_tls_ca_cert_file(cfg.m_tls_ca_cert_file), m_conn(nullptr),
      m_opts(nullptr), m_connected(false) {
  std::string auth_info;
  if (!m_username.empty())
    auth_info += " username=" + m_username;
  if (!m_token.empty())
    auth_info += " token=***";
  if (!m_credentials_file.empty())
    auth_info += " creds_file=" + m_credentials_file;
  if (m_enable_tls)
    auth_info += " tls=enabled";

  Logger::info("NATS client created: {}:{} subject={} queue_group={}{}",
               cfg.m_host, cfg.m_port, cfg.m_subject, cfg.m_queue_group,
               auth_info);
}

NatsClient::~NatsClient() {
  try {
    // Prevent any reconnect attempts from a concurrent thread while the
    // connection resources are being torn down.
    m_shutdown.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    cleanup();
    Logger::info("Disconnected from NATS server");
    // NOLINTNEXTLINE(bugprone-empty-catch) — destructor must not throw
  } catch (...) {
  }
}

bool NatsClient::connect() {
  if (m_connected && m_conn != nullptr) {
    return true;
  }

  const auto url = std::format("nats://{}:{}", m_host, m_port);
  bool first_attempt = true;

  while (true) {
    if (m_shutdown.load(std::memory_order_acquire)) {
      Logger::warn("NATS connect aborted: shutdown requested");
      return false;
    }

    std::lock_guard<std::mutex> lock(m_conn_mutex);

    if (m_connected && m_conn != nullptr) {
      return true;
    }

    try {
      cleanup();

      // Options calls all follow the same pattern: any non-NATS_OK status is
      // an unrecoverable setup failure — record it and abort the connect.
      const auto check_ok = [this](natsStatus status,
                                   const std::string &error_message) {
        if (status != NATS_OK) {
          set_error(error_message);
          return false;
        }
        return true;
      };

      if (!check_ok(natsOptions_Create(&m_opts),
                    "Failed to create NATS options")) {
        return false;
      }

      if (!check_ok(natsOptions_SetURL(m_opts, url.c_str()),
                    "Failed to set NATS URL: " + url)) {
        return false;
      }

      // Enable infinite reconnect attempts and log lifecycle callbacks
      if (!check_ok(natsOptions_SetAllowReconnect(m_opts, true),
                    "Failed to enable NATS reconnects")) {
        return false;
      }

      if (!check_ok(natsOptions_SetMaxReconnect(m_opts, -1),
                    "Failed to configure infinite NATS reconnect attempts")) {
        return false;
      }

      if (!check_ok(natsOptions_SetDisconnectedCB(
                        m_opts,
                        [](natsConnection *nc, void *closure) {
                          (void)nc;
                          auto *client = static_cast<NatsClient *>(closure);
                          if (client != nullptr) {
                            client->mark_disconnected(
                                "lost connection to NATS server");
                          }
                        },
                        this),
                    "Failed to set NATS disconnected callback")) {
        return false;
      }

      if (!check_ok(natsOptions_SetReconnectedCB(
                        m_opts,
                        [](natsConnection *nc, void *closure) {
                          auto *client = static_cast<NatsClient *>(closure);
                          if (client != nullptr) {
                            client->m_connected = true;

                            std::string connected_url_suffix;
                            if (nc != nullptr) {
                              char connected_url[256] = {0};
                              if (natsConnection_GetConnectedUrl(
                                      nc, connected_url,
                                      sizeof(connected_url)) == NATS_OK &&
                                  connected_url[0] != '\0') {
                                connected_url_suffix =
                                    std::string(": ") + connected_url;
                              }
                            }

                            Logger::info("NATS reconnected successfully{}",
                                         connected_url_suffix);
                          }
                        },
                        this),
                    "Failed to set NATS reconnected callback")) {
        return false;
      }

      if (!check_ok(natsOptions_SetErrorHandler(
                        m_opts,
                        [](natsConnection *nc, natsSubscription *sub,
                           natsStatus err, void *closure) {
                          (void)nc;
                          (void)sub;
                          auto *client = static_cast<NatsClient *>(closure);
                          const std::string error_text = nats_status_text(err);
                          if (client != nullptr) {
                            client->set_error("asynchronous NATS error: " +
                                              error_text);
                          } else {
                            Logger::error("NATS asynchronous error: {}",
                                          error_text);
                          }
                        },
                        this),
                    "Failed to set NATS error callback")) {
        return false;
      }

      if (!check_ok(natsOptions_SetRetryOnFailedConnect(m_opts, true, nullptr,
                                                        nullptr),
                    "Failed to enable NATS retry-on-failed-connect")) {
        return false;
      }

      if (!check_ok(natsOptions_SetReconnectWait(m_opts, 1000),
                    "Failed to set NATS reconnect wait")) {
        return false;
      }

      if (!check_ok(natsOptions_SetClosedCB(
                        m_opts,
                        [](natsConnection *nc, void *closure) {
                          (void)nc;
                          auto *client = static_cast<NatsClient *>(closure);
                          if (client != nullptr) {
                            client->m_connected = false;
                            Logger::error("NATS connection closed permanently");
                          }
                        },
                        this),
                    "Failed to set NATS closed callback")) {
        return false;
      }

      if (!check_ok(natsOptions_SetTimeout(m_opts, ms_to_seconds(m_timeout_ms)),
                    "Failed to set NATS timeout")) {
        return false;
      }

      if (!m_token.empty()) {
        if (!check_ok(natsOptions_SetToken(m_opts, m_token.c_str()),
                      "Failed to set NATS token")) {
          return false;
        }
        Logger::debug("NATS token authentication configured");
      } else if (!m_username.empty() || !m_password.empty()) {
        if (!check_ok(natsOptions_SetUserInfo(m_opts, m_username.c_str(),
                                              m_password.c_str()),
                      "Failed to set NATS username/password")) {
          return false;
        }
        Logger::debug("NATS username/password authentication configured");
      }

      if (!m_credentials_file.empty()) {
        if (!check_ok(natsOptions_SetUserCredentialsFromFiles(
                          m_opts, m_credentials_file.c_str(), nullptr),
                      "Failed to set NATS credentials file: " +
                          m_credentials_file)) {
          return false;
        }
        Logger::debug("NATS credentials file configured: {}",
                      m_credentials_file);
      }

      // Set TLS configuration
      if (m_enable_tls) {
        if (!check_ok(natsOptions_SetSecure(m_opts, true),
                      "Failed to enable NATS TLS")) {
          return false;
        }

        if (!m_tls_ca_cert_file.empty()) {
          if (!check_ok(natsOptions_LoadCATrustedCertificates(
                            m_opts, m_tls_ca_cert_file.c_str()),
                        "Failed to load NATS CA certificate: " +
                            m_tls_ca_cert_file)) {
            return false;
          }
        }

        if (!m_tls_cert_file.empty() && !m_tls_key_file.empty()) {
          if (!check_ok(
                  natsOptions_LoadCertificatesChain(
                      m_opts, m_tls_cert_file.c_str(), m_tls_key_file.c_str()),
                  "Failed to load NATS client certificates")) {
            return false;
          }
        }

        Logger::debug("NATS TLS configuration enabled");
      }

      if (first_attempt) {
        Logger::warn("Waiting for NATS server to become available at {}...",
                     url);
        first_attempt = false;
      }

      const natsStatus s = natsConnection_Connect(&m_conn, m_opts);
      if (s == NATS_OK) {
        m_connected = true;
        set_last_error("");
        Logger::info("Connected to NATS server: {}", url);
        return true;
      }

      m_connected = false;
      set_last_error("Failed to connect to NATS server: " + url + ": " +
                     nats_status_text(s));
      Logger::warn("{}; retrying in 1 second", get_last_error().value_or(""));
      cleanup();
    } catch (const std::exception &e) {
      m_connected = false;
      set_last_error(std::string("Exception in NATS connect: ") + e.what());
      Logger::warn("{}; retrying in 1 second", get_last_error().value_or(""));
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void NatsClient::disconnect() {
  std::lock_guard<std::mutex> lock(m_conn_mutex);
  disconnect_locked();
  Logger::info("Disconnected from NATS server");
}

void NatsClient::drain_subscription_locked(bool log_success) {
  for (auto &subscription : m_subscriptions) {
    if (!subscription.m_sub) {
      continue;
    }
    const natsStatus s = natsSubscription_Drain(subscription.m_sub);
    if (s != NATS_OK) {
      Logger::warn("NATS subscription drain failed for subject '{}': {}",
                   subscription.m_subject, nats_status_text(s));
    } else if (log_success) {
      Logger::info("NATS subscription drained successfully: {}",
                   subscription.m_subject);
    }
  }
}

void NatsClient::destroy_subscription_locked() {
  // Drain must have completed before this point (drain_subscription_locked
  // blocks until in-flight deliveries finish), so destroying the C
  // subscription and freeing the callback holder is safe.
  for (auto &subscription : m_subscriptions) {
    if (subscription.m_sub) {
      natsSubscription_Destroy(subscription.m_sub);
    }
    subscription.m_callback.reset();
  }
  m_subscriptions.clear();
}

void NatsClient::destroy_connection_locked() {
  if (m_conn) {
    natsConnection_Close(m_conn);
    natsConnection_Destroy(m_conn);
    m_conn = nullptr;
  }
}

void NatsClient::disconnect_locked() {
  // Drain the subscription first so any in-flight message callback completes
  // before the underlying NATS resources are destroyed (avoids use-after-free).
  drain_subscription_locked(false);
  destroy_subscription_locked();
  destroy_connection_locked();
  m_connected = false;
}

void NatsClient::cleanup() {
  // NOTE: caller must hold m_conn_mutex
  disconnect_locked();
  if (m_opts) {
    natsOptions_Destroy(m_opts);
    m_opts = nullptr;
  }
}

std::optional<std::string> NatsClient::request(const std::string &subject,
                                               const std::string &data,
                                               int timeout_ms) {
  const auto reply = request_impl(subject, data, {}, {}, timeout_ms);
  if (!reply) {
    return std::nullopt;
  }
  if (reply->m_data.empty()) {
    Logger::warn("NatsClient::request: empty reply data for subject '{}'",
                 subject);
    return std::nullopt;
  }
  return reply->m_data;
}

std::optional<NatsReply>
NatsClient::request_impl(const std::string &subject, const std::string &data,
                         const NatsHeaders &headers,
                         const std::vector<std::string> &reply_header_keys,
                         int timeout_ms) {
  if (!ensure_connected()) {
    return std::nullopt;
  }

  natsConnection *conn = acquire_connection("request");
  if (!conn) {
    return std::nullopt;
  }

  natsMsg *msg = nullptr;
  natsStatus s = natsMsg_Create(&msg, subject.c_str(), nullptr, data.c_str(),
                                static_cast<int>(data.size()));
  if (s != NATS_OK) {
    set_error("Failed to create NATS request message for subject '" + subject +
              "'");
    return std::nullopt;
  }

  if (!set_msg_headers(msg, headers, "NATS request message")) {
    return std::nullopt;
  }

  natsMsg *reply = nullptr;
  s = natsConnection_RequestMsg(&reply, conn, msg,
                                timeout_ms > 0 ? timeout_ms : m_timeout_ms);
  natsMsg_Destroy(msg);

  if (s != NATS_OK) {
    const std::string error_text = nats_status_text(s);

    if (s == NATS_NO_RESPONDERS) {
      set_last_error("request failed for subject '" + subject +
                     "': " + error_text);
      return std::nullopt;
    }

    mark_disconnected("request failed for subject '" + subject +
                      "': " + error_text);
    return std::nullopt;
  }

  NatsReply result;
  const char *reply_data = natsMsg_GetData(reply);
  const int reply_len = natsMsg_GetDataLength(reply);
  if (reply_data && reply_len > 0) {
    result.m_data.assign(reply_data, reply_len);
  }

  // Extract requested headers from the reply message
  for (const auto &key : reply_header_keys) {
    const char *value = nullptr;
    s = natsMsgHeader_Get(reply, key.c_str(), &value);
    if (s == NATS_OK && value) {
      result.m_headers[key] = value;
    }
  }

  natsMsg_Destroy(reply);
  return result;
}

std::pair<NatsReply, std::string>
NatsClient::request_with_consume_span_id(const std::string &subject,
                                         const std::string &data,
                                         int timeout_ms) {
  NatsReply reply = request_with_headers(
      subject, data, {}, {NatsContract::kConsumeSpanIdHeader}, timeout_ms);
  std::string consume_span_id;
  const auto it = reply.m_headers.find(NatsContract::kConsumeSpanIdHeader);
  if (it != reply.m_headers.end() && !it->second.empty()) {
    consume_span_id = it->second;
  }
  return {std::move(reply), std::move(consume_span_id)};
}

bool NatsClient::publish(const std::string &subject, const std::string &data) {
  if (!ensure_connected()) {
    return false;
  }

  natsConnection *conn = acquire_connection("publish");
  if (!conn) {
    return false;
  }

  natsStatus s =
      natsConnection_PublishString(conn, subject.c_str(), data.c_str());
  if (s != NATS_OK) {
    mark_disconnected("publish failed for subject '" + subject +
                      "': " + nats_status_text(s));
    return false;
  }

  return true;
}

bool NatsClient::subscribe(const std::string &subject,
                           NatsMessageCallback callback,
                           const std::string &queue_group) {
  if (!ensure_connected()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_conn_mutex);

  if (!m_conn) {
    mark_disconnected("subscribe: connection is null");
    return false;
  }

  NatsSubscription subscription;
  subscription.m_callback =
      std::make_unique<std::shared_ptr<NatsMessageCallback>>(
          std::make_shared<NatsMessageCallback>(std::move(callback)));
  subscription.m_subject = subject;

  natsSubscription *sub = nullptr;
  natsStatus s;
  if (queue_group.empty()) {
    s = natsConnection_Subscribe(&sub, m_conn, subject.c_str(),
                                 nats_message_callback,
                                 subscription.m_callback.get());
  } else {
    s = natsConnection_QueueSubscribe(
        &sub, m_conn, subject.c_str(), queue_group.c_str(),
        nats_message_callback, subscription.m_callback.get());
  }

  if (s != NATS_OK) {
    set_error("NATS subscribe failed for subject: " + subject + ": " +
              nats_status_text(s));
    return false;
  }

  subscription.m_sub = sub;
  m_subscriptions.push_back(std::move(subscription));

  std::string queue_group_info =
      queue_group.empty() ? "" : " queue_group=" + queue_group;
  Logger::info("Subscribed to NATS subject: {}{}", subject, queue_group_info);
  return true;
}

bool NatsClient::subscribe_queue(const std::string &subject,
                                 const std::string &queue_group,
                                 NatsMessageCallback callback) {
  return subscribe(subject, std::move(callback), queue_group);
}

void NatsClient::unsubscribe() {
  std::lock_guard<std::mutex> lock(m_conn_mutex);
  if (m_subscriptions.empty()) {
    return;
  }
  const size_t count = m_subscriptions.size();
  // Drain so in-flight handlers finish before the subscriptions are destroyed.
  drain_subscription_locked(false);
  destroy_subscription_locked();
  Logger::info("Unsubscribed from {} NATS subject(s)", count);
}

bool NatsClient::drain(int timeout_ms) {
  std::lock_guard<std::mutex> lock(m_conn_mutex);

  // Drain subscription: waits for in-flight message handlers to complete
  drain_subscription_locked(true);

  // Drain connection: flushes all pending publishes and waits for
  // acknowledgements
  if (m_conn) {
    const natsStatus s =
        natsConnection_DrainTimeout(m_conn, ms_to_seconds(timeout_ms));
    if (s != NATS_OK) {
      Logger::warn("NATS connection drain failed: {}", nats_status_text(s));
    } else {
      Logger::info("NATS connection drained successfully");
    }
  }

  destroy_subscription_locked();
  destroy_connection_locked();
  m_connected = false;

  Logger::info("NATS client drain complete");
  return true;
}

bool NatsClient::check_connection() {
  if (!ensure_connected()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_conn_mutex);

  if (!m_conn) {
    mark_disconnected("health check: connection is null");
    return false;
  }

  const natsConnStatus status = natsConnection_Status(m_conn);
  if (status == NATS_CONN_STATUS_CLOSED ||
      status == NATS_CONN_STATUS_DISCONNECTED ||
      status == NATS_CONN_STATUS_RECONNECTING) {
    m_connected = false;
    set_last_error("health check detected non-ready NATS connection status: " +
                   std::to_string(static_cast<int>(status)));
    const auto last_error = get_last_error();
    Logger::warn("NATS health check: {}", last_error.value_or(""));
    return false;
  }

  return true;
}

std::optional<std::string> NatsClient::ping() {
  if (!check_connection()) {
    return std::nullopt;
  }
  return std::string("PONG");
}

std::optional<std::string> NatsClient::get_last_error() const {
  std::lock_guard<std::mutex> lock(m_error_mutex);
  return m_last_error.empty() ? std::nullopt : std::optional(m_last_error);
}

bool NatsClient::set_msg_headers(natsMsg *msg, const NatsHeaders &headers,
                                 const std::string &operation) {
  for (const auto &[key, value] : headers) {
    const natsStatus s = natsMsgHeader_Set(msg, key.c_str(), value.c_str());
    if (s != NATS_OK) {
      natsMsg_Destroy(msg);
      std::string err = "Failed to set header '";
      err += key;
      err += "' on ";
      err += operation;
      set_error(err);
      return false;
    }
  }
  return true;
}

bool NatsClient::publish_with_headers(const std::string &subject,
                                      const std::string &data,
                                      const NatsHeaders &headers) {
  if (!ensure_connected()) {
    return false;
  }

  natsConnection *conn = acquire_connection("publish_with_headers");
  if (!conn) {
    return false;
  }

  natsMsg *msg = nullptr;
  natsStatus s = natsMsg_Create(&msg, subject.c_str(), nullptr, data.c_str(),
                                static_cast<int>(data.size()));
  if (s != NATS_OK) {
    set_error("Failed to create NATS message for subject '" + subject + "'");
    return false;
  }

  if (!set_msg_headers(msg, headers, "NATS message")) {
    return false;
  }

  s = natsConnection_PublishMsg(conn, msg);
  natsMsg_Destroy(msg);

  if (s != NATS_OK) {
    mark_disconnected("publish_with_headers failed for subject '" + subject +
                      "': " + nats_status_text(s));
    return false;
  }

  return true;
}

NatsReply NatsClient::request_with_headers(
    const std::string &subject, const std::string &data,
    const NatsHeaders &headers,
    const std::vector<std::string> &reply_header_keys, int timeout_ms) {
  const auto reply =
      request_impl(subject, data, headers, reply_header_keys, timeout_ms);
  return reply.value_or(NatsReply{});
}

void NatsClient::set_error(const std::string &error) {
  set_last_error(error);
  Logger::error("NATS error: {}", error);
}

void NatsClient::set_last_error(const std::string &error) {
  std::lock_guard<std::mutex> lock(m_error_mutex);
  m_last_error = error;
}

natsConnection *NatsClient::acquire_connection(const std::string &operation) {
  std::lock_guard<std::mutex> lock(m_conn_mutex);
  if (!m_conn) {
    mark_disconnected(operation + ": connection is null");
    return nullptr;
  }
  return m_conn;
}

bool NatsClient::ensure_connected() {
  if (m_shutdown.load(std::memory_order_acquire)) {
    return false;
  }
  if (m_connected && m_conn != nullptr) {
    return true;
  }

  Logger::warn("NATS client is disconnected. Reconnecting and waiting for NATS "
               "server availability...");
  return connect();
}

bool NatsClient::mark_disconnected(const std::string &reason) {
  const bool was_connected = m_connected.exchange(false);
  set_last_error(reason);

  if (was_connected) {
    Logger::error("NATS connection lost: {}", reason);
    Logger::warn(
        "NATS auto-reconnect is active. Waiting for server recovery...");
  } else {
    Logger::warn("NATS still unavailable: {}", reason);
  }

  return false;
}
