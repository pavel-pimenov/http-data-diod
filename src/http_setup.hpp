#ifndef HTTP_SETUP_HPP
#define HTTP_SETUP_HPP

#include "httplib/httplib.h"
#include <string>

// Connection-wide httplib tuning shared by every HTTP client in the project.
// Split out of the common_utils.hpp umbrella so the plaintext/SSL setup path
// can include just this header (definition of setup_ssl_client lives in
// common_utils.cpp).

void setup_ssl_client(httplib::SSLClient &client, int timeout_seconds,
                      bool enable_cert_verification,
                      bool enable_hostname_verification,
                      const std::string &ca_cert_path,
                      bool enable_connection_reuse);

// Applies the connection-wide timeouts and keep-alive tuning shared by the
// plaintext httplib::Client and the SSLClient. setup_ssl_client augments this
// with certificate/hostname verification. Both classes share these setters via
// ClientImpl, so a single template avoids hand-writing the identical block in
// the SSL and plaintext setup paths.
template <typename ClientT>
void setup_http_connection(ClientT &client, int timeout_seconds,
                           bool enable_connection_reuse) {
  client.set_connection_timeout(5, 0);
  client.set_read_timeout(timeout_seconds, 0);
  client.set_write_timeout(timeout_seconds, 0);
  if (enable_connection_reuse) {
    client.set_keep_alive(true);
    client.set_tcp_nodelay(true);
  }
}

#endif // HTTP_SETUP_HPP
