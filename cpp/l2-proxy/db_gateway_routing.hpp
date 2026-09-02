#ifndef DB_GATEWAY_ROUTING_HPP
#define DB_GATEWAY_ROUTING_HPP

#include <format>
#include <string>

// Pure helpers of the /v1/sql routing in the proxy request handler. Header-only
// and free of AppContext/NATS/handlers so the proxy-core unit tests can cover
// the routing contract (path shape, list vs query/ping, 405 vs 404) without
// booting the whole stack.

namespace db_gateway_routing {

// Normalizes the part of a /v1/sql path after the prefix: trims leading and
// trailing slashes so "/v1/sql/" behaves like "/v1/sql" and "/v1/sql//oracle/"
// like "/oracle".
inline std::string normalize_path_rest(std::string path_rest) {
  while (!path_rest.empty() && path_rest.front() == '/') {
    path_rest.erase(0, 1);
  }
  while (!path_rest.empty() && path_rest.back() == '/') {
    path_rest.pop_back();
  }
  return path_rest;
}

// Parsed /v1/sql path rest. m_is_list matches the empty path (GET /v1/sql
// listing); m_valid is false when the rest has no single {db}/{action} shape
// (e.g. "/oracle" without an action or "/oracle/ping/extra").
struct ParsedPath {
  std::string m_db_name;
  std::string m_action;
  bool m_is_list = false;
  bool m_valid = true;
};

inline ParsedPath parse_path(const std::string &normalized_rest) {
  if (normalized_rest.empty()) {
    return {std::string{}, std::string{}, true, true};
  }
  const size_t slash = normalized_rest.find('/');
  std::string db_name = normalized_rest.substr(0, slash);
  std::string action = slash == std::string::npos
                           ? std::string{}
                           : normalized_rest.substr(slash + 1);
  const bool valid =
      !db_name.empty() && !action.empty() &&
      action.find('/') == std::string::npos;
  return {std::move(db_name), std::move(action), false, valid};
}

// Decision for a known database's action/method pair: a known action reached
// with an unsupported HTTP method is a client 405; a totally unknown action is
// a 404; the exact matches (query+POST / ping+GET) are not an error.
struct MethodDecision {
  bool m_is_error = false;
  int m_status = 0;
  std::string m_code;
  std::string m_message;
};

inline MethodDecision classify_method(const std::string &action,
                                      const std::string &method) {
  if ((action == "query" && method != "POST") ||
      (action == "ping" && method != "GET")) {
    return {true, 405, "METHOD_NOT_ALLOWED",
            std::format("Unsupported method '{}' for action '{}'", method,
                        action)};
  }
  if (action != "query" && action != "ping") {
    return {true, 404, "NOT_FOUND",
            std::format("Unsupported DB gateway action '{}' for {}", action,
                        method)};
  }
  return {};
}

} // namespace db_gateway_routing

#endif // DB_GATEWAY_ROUTING_HPP