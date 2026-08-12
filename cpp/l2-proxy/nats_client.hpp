#ifndef NATS_CLIENT_HPP
#define NATS_CLIENT_HPP

#include "interfaces.hpp"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nats/nats.h>

// Configuration bundle for NatsClient
struct NatsConfig {
  std::string m_host;
  int m_port = 0;
  std::string m_subject;
  std::string m_queue_group;
  int m_timeout_ms = 30000;
  std::string m_username;
  std::string m_password;
  std::string m_token;
  std::string m_credentials_file;
  bool m_enable_tls = false;
  std::string m_tls_cert_file;
  std::string m_tls_key_file;
  std::string m_tls_ca_cert_file;
};

using NatsHeaders = std::map<std::string, std::string>;

struct NatsReply {
  std::string m_data;
  NatsHeaders m_headers;
};

using NatsMessageCallback =
    std::function<void(const std::string &, const std::string &,
                       const std::string &)>;

// One active subscription. The callback is kept in a shared_ptr that is itself
// heap-allocated: the C NATS delivery handler receives a pointer to that
// shared_ptr (address stays stable across vector reallocations) and copies it
// while a message is being processed, so the callback object outlives both the
// subscription teardown and any in-flight delivery.
struct NatsSubscription {
  natsSubscription *m_sub = nullptr;
  std::unique_ptr<std::shared_ptr<NatsMessageCallback>> m_callback;
  std::string m_subject;
};

// NATS client for request/response messaging pattern
class NatsClient : public IConnectableClient {
public:
  explicit NatsClient(const NatsConfig &cfg);

  ~NatsClient() override;

  [[nodiscard]] bool connect();

  void disconnect();

  // IConnectableClient interface
  bool is_connected() const override { return m_connected; }

  // NATS-specific methods

  // Send request and wait for response (synchronous)
  // Returns nullopt on connection/error, empty string on valid empty reply
  std::optional<std::string> request(const std::string &subject,
                                     const std::string &data,
                                     int timeout_ms = 0);

  // Send request with headers, receive reply with headers
  // reply_header_keys: list of header keys to extract from the reply (if empty,
  // extracts none)
  NatsReply
  request_with_headers(const std::string &subject, const std::string &data,
                       const NatsHeaders &headers = {},
                       const std::vector<std::string> &reply_header_keys = {},
                       int timeout_ms = 0);

  // Send request and return the reply plus the worker's consume span id
  // extracted from the reply headers (empty when absent). Shared by the poll
  // service and the DB gateway, which repeated request_with_headers + header
  // lookup.
  std::pair<NatsReply, std::string>
  request_with_consume_span_id(const std::string &subject,
                               const std::string &data, int timeout_ms = 0);

  // Publish message (fire-and-forget)
  bool publish(const std::string &subject, const std::string &data);

  // Publish message with headers
  bool publish_with_headers(const std::string &subject, const std::string &data,
                            const NatsHeaders &headers);

  // Subscribe to subject with callback
  // Callback arguments: subject, data, reply_to
  bool subscribe(const std::string &subject, NatsMessageCallback callback,
                 const std::string &queue_group = "");

  // Subscribe with queue group for load balancing
  bool subscribe_queue(const std::string &subject,
                       const std::string &queue_group,
                       NatsMessageCallback callback);

  // Unsubscribes (and destroys) all active subscriptions.
  void unsubscribe();

  // Drain subscription and connection — waits for in-flight messages
  // timeout_ms: maximum time to wait for drain to complete (0 = no wait)
  bool drain(int timeout_ms = 5000);

  // Health check (returns bool)
  bool check_connection();

  // Ping NATS server (nullopt = no connection)
  std::optional<std::string> ping();

  // Get last error (nullopt if no error recorded)
  std::optional<std::string> get_last_error() const;

private:
  std::string m_host;
  int m_port;
  std::string m_subject;
  std::string m_queue_group;
  int m_timeout_ms;

  // Authentication fields
  std::string m_username;
  std::string m_password;
  std::string m_token;
  std::string m_credentials_file;
  bool m_enable_tls;
  std::string m_tls_cert_file;
  std::string m_tls_key_file;
  std::string m_tls_ca_cert_file;

  natsConnection *m_conn;
  natsOptions *m_opts;
  std::vector<NatsSubscription> m_subscriptions;

  std::atomic<bool> m_connected;
  std::atomic<bool> m_shutdown{false};
  // The NATS async-callback thread delivers the Closed callback
  // asynchronously after each of our connections is destroyed. Keeping a
  // simple "closed" flag is not enough: a worker thread may open a new
  // connection while the pool is being drained, so several Closed callbacks
  // can be in flight at once (one per created connection). We therefore count
  // created connections and delivered Closed callbacks; the destructor blocks
  // until every created connection has delivered its Closed callback, so no
  // connection-level callback can dereference `this` after the object dies.
  std::atomic<uint64_t> m_connected_instances{0};
  std::atomic<uint64_t> m_closed_callbacks_delivered{0};
  std::mutex m_conn_mutex;
  mutable std::mutex m_error_mutex;
  std::string m_last_error;

  bool setup_options();
  // Teardown helpers — caller must hold m_conn_mutex
  void drain_subscription_locked(bool log_success);
  void destroy_subscription_locked();
  void destroy_connection_locked();
  void disconnect_locked();
  void cleanup();
  // Blocks until the asynchronously delivered Closed callback has run. The
  // Closed callback is the last callback a connection fires, so waiting for it
  // also guarantees the Disconnected/Reconnected/Error callbacks (queued
  // before it) have been delivered. Bounded to avoid hanging on a connection
  // that never opened.
  void wait_for_closed_callback();
  // Copy the live connection pointer under the mutex; marks the client
  // disconnected (with the operation name) when the connection is gone.
  natsConnection *acquire_connection(const std::string &operation);
  // Shared request path used by request()/request_with_headers()
  std::optional<NatsReply>
  request_impl(const std::string &subject, const std::string &data,
               const NatsHeaders &headers,
               const std::vector<std::string> &reply_header_keys,
               int timeout_ms);
  void set_error(const std::string &error);
  void set_last_error(const std::string &error);
  bool ensure_connected();
  bool mark_disconnected(const std::string &reason);
  // Sets caller-specified headers on a NATS message; on failure destroys the
  // message, records the error (with the operation name), returns false.
  bool set_msg_headers(natsMsg *msg, const NatsHeaders &headers,
                       const std::string &operation);

  // Convert timeout
  static int ms_to_seconds(int ms) { return (ms + 999) / 1000; }
};

#endif // NATS_CLIENT_HPP