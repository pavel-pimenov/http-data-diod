#include "http_client.hpp"
#include "common_utils.hpp"
#include "logger.hpp"

std::atomic<int> HttpClient::g_instance_count{0};
std::atomic<uint64_t> HttpClient::g_total_created{0};
std::atomic<uint64_t> HttpClient::g_total_destroyed{0};

HttpClient::HttpClient(const int timeout_seconds,
                       const bool enable_connection_reuse,
                       bool enable_ssl_server_certificate_verification,
                       bool enable_ssl_server_hostname_verification,
                       const std::string &ssl_ca_cert_path)
    : m_timeout_seconds(timeout_seconds),
      m_enable_connection_reuse(enable_connection_reuse),
      m_enable_ssl_server_certificate_verification(
          enable_ssl_server_certificate_verification),
      m_enable_ssl_server_hostname_verification(
          enable_ssl_server_hostname_verification),
      m_ssl_ca_cert_path(ssl_ca_cert_path), m_last_status_code(0),
      m_is_valid(true), m_last_used(std::chrono::steady_clock::now()) {
  ++g_instance_count;
  ++g_total_created;
  Logger::debug("new HttpClient created (active: {}, total: {}), timeout: {}s",
                g_instance_count.load(), g_total_created.load(),
                timeout_seconds);
}

HttpClient::~HttpClient() {
  try {
    --g_instance_count;
    ++g_total_destroyed;
    Logger::debug("delete HttpClient (active: {}, created: {}, destroyed: {})",
                  g_instance_count.load(), g_total_created.load(),
                  g_total_destroyed.load());
    // Destructor must not throw
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (...) {
  }
}

void HttpClient::setup_client(const ParsedUrl &parsed_url) {
  // Validate host and port before creating client
  if (parsed_url.m_host.empty()) {
    throw std::runtime_error("Invalid URL: host is empty");
  }
  if (parsed_url.m_port <= 0 || parsed_url.m_port > 65535) {
    throw std::runtime_error("Invalid URL: port out of range");
  }

  if (parsed_url.m_is_https) {
    m_ssl_client = std::make_unique<httplib::SSLClient>(parsed_url.m_host,
                                                        parsed_url.m_port);
    setup_ssl_client(*m_ssl_client, m_timeout_seconds,
                     m_enable_ssl_server_certificate_verification,
                     m_enable_ssl_server_hostname_verification,
                     m_ssl_ca_cert_path, m_enable_connection_reuse);
  } else {
    m_client =
        std::make_unique<httplib::Client>(parsed_url.m_host, parsed_url.m_port);
    setup_http_connection(*m_client, m_timeout_seconds,
                          m_enable_connection_reuse);
  }
}

HttpClient::PreparedRequest
HttpClient::prepare_request(const std::string &url, const std::string &body,
                            const std::string &traceparent,
                            const httplib::Headers &additional_headers) {
  ParsedUrl parsed_url = parse_url(url);

  parsed_url.m_path = normalize_path(parsed_url.m_path);

  if ((parsed_url.m_is_https && !m_ssl_client) ||
      (!parsed_url.m_is_https && !m_client)) {
    setup_client(parsed_url);
  }

  httplib::Headers headers;
  if (!body.empty()) {
    headers.emplace("Content-Length", std::to_string(body.size()));
  }
  for (const auto &header : additional_headers) {
    headers.emplace(header.first, header.second);
  }
  if (!traceparent.empty()) {
    headers.emplace("traceparent", traceparent);
  }

  return {parsed_url, headers};
}

HttpResponse HttpClient::execute_request(const PreparedRequest &req,
                                         const std::string &body,
                                         const std::string &operation) {
  httplib::Result result;
  if (req.m_parsed_url.m_is_https) {
    result = body.empty()
                 ? m_ssl_client->Get(req.m_parsed_url.m_path, req.m_headers)
                 : m_ssl_client->Post(req.m_parsed_url.m_path, req.m_headers,
                                      body, "application/json");
  } else {
    result = body.empty()
                 ? m_client->Get(req.m_parsed_url.m_path, req.m_headers)
                 : m_client->Post(req.m_parsed_url.m_path, req.m_headers, body,
                                  "application/json");
  }

  if (!result) {
    m_last_status_code = 0;
    throw std::runtime_error(
        format_http_error(result.error(), m_timeout_seconds, operation));
  }

  m_last_status_code = result->status;
  return {result->body, result->headers, result->status};
}

HttpResponse HttpClient::post(const std::string &url, const std::string &body,
                              const std::string &traceparent,
                              const httplib::Headers &additional_headers) {
  Logger::debug("Calling setup_httplib_post for POST request to: {}", url);
  touch();
  PreparedRequest req =
      prepare_request(url, body, traceparent, additional_headers);
  return execute_request(req, body, "POST");
}

HttpResponse HttpClient::get(const std::string &url,
                             const std::string &traceparent,
                             const httplib::Headers &additional_headers) {
  Logger::debug("Calling setup_httplib_get for GET request to: {}", url);
  touch();
  PreparedRequest req =
      prepare_request(url, "", traceparent, additional_headers);
  return execute_request(req, "", "GET");
}

void HttpClient::post_no_response(const std::string &url,
                                  const std::string &body,
                                  const std::string &traceparent,
                                  const httplib::Headers &additional_headers) {
  PreparedRequest req =
      prepare_request(url, body, traceparent, additional_headers);
  execute_request(req, body, "POST");
}

bool HttpClient::is_valid() const { return m_is_valid; }

void HttpClient::invalidate() { m_is_valid = false; }

int HttpClient::get_last_status_code() const { return m_last_status_code; }

std::chrono::steady_clock::time_point HttpClient::get_last_used() const {
  return m_last_used;
}

void HttpClient::touch() { m_last_used = std::chrono::steady_clock::now(); }
