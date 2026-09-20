#ifndef JSON_SCHEMA_VALIDATOR_HPP
#define JSON_SCHEMA_VALIDATOR_HPP

#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "json_utils.hpp"

// Request Validator
// Validates L2 server requests
//
// NOTE: get_body_response_ref is declared in json_utils.hpp (included above).
class RequestValidator {
private:
#if __has_include(<flat_set>) && defined(__cpp_lib_flat_set)
  using SmallStringSet = std::flat_set<std::string>;
#else
  using SmallStringSet = std::unordered_set<std::string>;
#endif
  struct Allowed {
    SmallStringSet m_required_fields;
    SmallStringSet m_allowed_methods;
    SmallStringSet m_allowed_paths;
  } m_allowed;
  struct Limits {
    size_t m_max_body_size;
    size_t m_max_path_length;
  } m_limits;

public:
  RequestValidator()
      : m_allowed(),
        m_limits{static_cast<size_t>(10) * 1024 * 1024, 2048} // 10MB, 2KB defaults
  {}

  RequestValidator &add_required_field(const std::string &field) {
    m_allowed.m_required_fields.insert(field);
    return *this;
  }

  RequestValidator &add_allowed_method(const std::string &method) {
    m_allowed.m_allowed_methods.insert(method);
    return *this;
  }

  RequestValidator &add_allowed_path(const std::string &path_prefix) {
    m_allowed.m_allowed_paths.insert(path_prefix);
    return *this;
  }

  RequestValidator &set_max_body_size(size_t bytes) {
    m_limits.m_max_body_size = bytes;
    return *this;
  }

  RequestValidator &set_max_path_length(size_t length) {
    m_limits.m_max_path_length = length;
    return *this;
  }

  bool validate(const json &request, std::string &error) const {
    for (const auto &field : m_allowed.m_required_fields) {
      if (request.find(field) == request.end()) {
        error = "Missing required field: " + field;
        return false;
      }
    }

    if (request.contains("method")) {
      const std::string &method = request["method"];
      if (!m_allowed.m_allowed_methods.empty() &&
#if __has_include(<flat_set>) && defined(__cpp_lib_flat_set)
          !m_allowed.m_allowed_methods.contains(method)) {
#else
          m_allowed.m_allowed_methods.find(method) ==
              m_allowed.m_allowed_methods.end()) {
#endif
        error = "Method not allowed: " + method;
        return false;
      }
    }

    // Check path if present
    if (request.contains("path")) {
      const std::string &path = request["path"];

      if (path.length() > m_limits.m_max_path_length) {
        error = std::format("Path too long: {} > {}", path.length(),
                            m_limits.m_max_path_length);
        return false;
      }

      if (!m_allowed.m_allowed_paths.empty()) {
        // ranges::any_of — C++23 сахар вместо ручного цикла
        const bool path_allowed =
            std::ranges::any_of(m_allowed.m_allowed_paths,
                                [&](const auto &prefix) {
                                  return path.starts_with(prefix);
                                });
        if (!path_allowed) {
          error = "Path not allowed: " + path;
          return false;
        }
      }
    }

    // Check body if present
    if (request.contains("body")) {
      const std::string &body = request["body"];
      if (body.length() > m_limits.m_max_body_size) {
        error = std::format("Body too large: {} > {}", body.length(),
                            m_limits.m_max_body_size);
        return false;
      }
    }

    return true;
  }

  void validate_or_throw(const json &request) const {
    std::string error;
    if (!validate(request, error)) {
      throw std::invalid_argument(error);
    }
  }
};

// Response Validator
// Validates L2 server responses
class ResponseValidator {
private:
#if __has_include(<flat_set>) && defined(__cpp_lib_flat_set)
  using SmallIntSet = std::flat_set<int>;
#else
  using SmallIntSet = std::unordered_set<int>;
#endif
  struct Allowed {
    SmallIntSet m_status_codes;
  } m_allowed;
  struct Limits {
    bool m_require_body;
    size_t m_max_body_size;
  } m_limits;

public:
  ResponseValidator()
      : m_limits({false, static_cast<size_t>(50) * 1024 * 1024}) // 50MB default
  {}

  ResponseValidator &add_allowed_status_code(int code) {
    m_allowed.m_status_codes.insert(code);
    return *this;
  }

  ResponseValidator &require_body(bool required = true) {
    m_limits.m_require_body = required;
    return *this;
  }

  ResponseValidator &set_max_body_size(size_t bytes) {
    m_limits.m_max_body_size = bytes;
    return *this;
  }

  bool validate(const json &response, std::string &error) const {
    if (response.contains("status_code")) {
      int status = response["status_code"];
      if (!m_allowed.m_status_codes.empty() &&
#if __has_include(<flat_set>) && defined(__cpp_lib_flat_set)
          !m_allowed.m_status_codes.contains(status)) {
#else
          m_allowed.m_status_codes.find(status) ==
              m_allowed.m_status_codes.end()) {
#endif
        error = std::format("Status code not allowed: {}", status);
        return false;
      }
    }

    // Check body if required
    if (m_limits.m_require_body) {
      if (!response.contains("body")) {
        error = "Response body required but missing";
        return false;
      }

      const std::string &body_str = get_body_response_ref(response);
      if (body_str.length() > m_limits.m_max_body_size) {
        error = std::format("Response body too large: {} > {}",
                            body_str.length(), m_limits.m_max_body_size);
        return false;
      }
    }

    return true;
  }

  void validate_or_throw(const json &response) const {
    std::string error;
    if (!validate(response, error)) {
      throw std::invalid_argument(error);
    }
  }
};

inline RequestValidator create_standard_request_validator() {
  RequestValidator validator;
  validator.add_required_field("method")
      .add_required_field("path")
      .add_required_field("request_id")
      .add_allowed_method("GET")
      .add_allowed_method("POST")
      .set_max_body_size(static_cast<size_t>(10) * 1024 * 1024) // 10MB
      .set_max_path_length(2048);
  return validator;
}

inline ResponseValidator create_standard_response_validator() {
  ResponseValidator validator;
  validator.require_body(true).set_max_body_size(static_cast<size_t>(50) *
                                                 1024 * 1024); // 50MB
  return validator;
}

#endif // JSON_SCHEMA_VALIDATOR_HPP
