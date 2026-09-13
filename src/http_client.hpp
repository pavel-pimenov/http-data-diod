#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "cpp_httplib_config.h"
#include "httplib/httplib.h"
#include "url_utils.hpp"
#include <nlohmann/json.hpp>

struct HttpResponse {
  std::string m_body;
  httplib::Headers m_headers;
  int m_status;
};

// Builds a standard {"error": ...} JSON object. Shared by the worker paths
// that previously hand-rolled the raw JSON string for each failure.
inline nlohmann::json make_error_json(const std::string &message) {
  nlohmann::json body;
  body["error"] = message;
  return body;
}

// Builds an HttpResponse carrying a JSON error body. Replaces the duplicated
// `HttpResponse{R"(...)", headers, status}` literals in the worker.
inline HttpResponse make_error_response(int status,
                                        const std::string &message) {
  return HttpResponse{make_error_json(message).dump(), httplib::Headers{},
                      status};
}

class HttpClient {
public:
  static std::atomic<int> g_instance_count;
  static std::atomic<uint64_t> g_total_created;
  static std::atomic<uint64_t> g_total_destroyed;

  explicit HttpClient(int timeout_seconds = 10,
                      bool enable_connection_reuse = false,
                      bool enable_ssl_server_certificate_verification = false,
                      bool enable_ssl_server_hostname_verification = false,
                      const std::string &ssl_ca_cert_path = "");
  ~HttpClient();

  HttpResponse post(const std::string &url, const std::string &body,
                    const std::string &traceparent = "",
                    const httplib::Headers &additional_headers = {});
  HttpResponse get(const std::string &url, const std::string &traceparent = "",
                   const httplib::Headers &additional_headers = {});
  void post_no_response(const std::string &url, const std::string &body,
                        const std::string &traceparent = "",
                        const httplib::Headers &additional_headers = {});
  bool is_valid() const;
  void invalidate();
  int get_last_status_code() const;
  std::chrono::steady_clock::time_point get_last_used() const;
  void touch();

private:
  // cpp-httplib implementation
  struct PreparedRequest {
    ParsedUrl m_parsed_url;
    httplib::Headers m_headers;
  };
  void setup_client(const ParsedUrl &parsed_url);
  PreparedRequest prepare_request(const std::string &url,
                                  const std::string &body,
                                  const std::string &traceparent,
                                  const httplib::Headers &additional_headers);
  HttpResponse execute_request(const PreparedRequest &req,
                               const std::string &body,
                               const std::string &operation);
  std::unique_ptr<httplib::Client> m_client;
  std::unique_ptr<httplib::SSLClient> m_ssl_client;
  int m_timeout_seconds;
  bool m_enable_connection_reuse;
  bool m_enable_ssl_server_certificate_verification;
  bool m_enable_ssl_server_hostname_verification;
  std::string m_ssl_ca_cert_path;
  int m_last_status_code;
  bool m_is_valid;
  std::chrono::steady_clock::time_point m_last_used;
};

#endif // HTTP_CLIENT_HPP
