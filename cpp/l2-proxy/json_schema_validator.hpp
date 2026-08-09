#ifndef JSON_SCHEMA_VALIDATOR_HPP
#define JSON_SCHEMA_VALIDATOR_HPP

#include "logger.hpp"
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

// JSON Schema Validator
// Validates JSON requests against a defined schema
//
// Usage:
//   RequestValidator validator;
//   validator.add_required_field("method");
//   validator.add_required_field("path");
//   validator.add_allowed_method("GET");
//   validator.add_allowed_method("POST");
//
//   std::string error;
//   if (!validator.validate(request_json, error)) {
//       // Return 400 Bad Request with error
//   }

class RequestValidator {
private:
  std::unordered_set<std::string> m_required_fields;
  std::unordered_set<std::string> m_allowed_methods;
  std::unordered_set<std::string> m_allowed_paths;
  size_t m_max_body_size;
  size_t m_max_path_length;

public:
  RequestValidator()
      : m_max_body_size(static_cast<size_t>(10) * 1024 * 1024) // 10MB default
        ,
        m_max_path_length(2048) // 2KB default
  {}

  RequestValidator &add_required_field(const std::string &field) {
    m_required_fields.insert(field);
    return *this;
  }

  RequestValidator &add_allowed_method(const std::string &method) {
    m_allowed_methods.insert(method);
    return *this;
  }

  RequestValidator &add_allowed_path(const std::string &path_prefix) {
    m_allowed_paths.insert(path_prefix);
    return *this;
  }

  RequestValidator &set_max_body_size(size_t bytes) {
    m_max_body_size = bytes;
    return *this;
  }

  RequestValidator &set_max_path_length(size_t length) {
    m_max_path_length = length;
    return *this;
  }

  bool validate(const json &request, std::string &error) const {
    // Check required fields
    for (const auto &field : m_required_fields) {
      if (request.find(field) == request.end()) {
        error = "Missing required field: " + field;
        return false;
      }
    }

    // Check method if present
    if (request.contains("method")) {
      const std::string &method = request["method"];
      if (!m_allowed_methods.empty() &&
          m_allowed_methods.find(method) == m_allowed_methods.end()) {
        error = "Method not allowed: " + method;
        return false;
      }
    }

    // Check path if present
    if (request.contains("path")) {
      const std::string &path = request["path"];

      if (path.length() > m_max_path_length) {
        error = std::format("Path too long: {} > {}", path.length(),
                            m_max_path_length);
        return false;
      }

      if (!m_allowed_paths.empty()) {
        bool path_allowed = false;
        for (const auto &prefix : m_allowed_paths) {
          if (path.find(prefix) == 0) {
            path_allowed = true;
            break;
          }
        }
        if (!path_allowed) {
          error = "Path not allowed: " + path;
          return false;
        }
      }
    }

    // Check body size if present
    if (request.contains("body")) {
      const std::string &body = request["body"];
      if (body.length() > m_max_body_size) {
        error = std::format("Body too large: {} > {}", body.length(),
                            m_max_body_size);
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
  std::unordered_set<int> m_allowed_status_codes;
  bool m_require_body;
  size_t m_max_body_size;

public:
  ResponseValidator()
      : m_require_body(false),
        m_max_body_size(static_cast<size_t>(50) * 1024 * 1024) // 50MB default
  {}

  ResponseValidator &add_allowed_status_code(int code) {
    m_allowed_status_codes.insert(code);
    return *this;
  }

  ResponseValidator &require_body(bool required = true) {
    m_require_body = required;
    return *this;
  }

  ResponseValidator &set_max_body_size(size_t bytes) {
    m_max_body_size = bytes;
    return *this;
  }

  bool validate(const json &response, std::string &error) const {
    // Check status code
    if (response.contains("status_code")) {
      int status = response["status_code"];
      if (!m_allowed_status_codes.empty() &&
          m_allowed_status_codes.find(status) == m_allowed_status_codes.end()) {
        error = std::format("Status code not allowed: {}", status);
        return false;
      }
    }

    // Check body if required
    if (m_require_body) {
      if (!response.contains("body")) {
        error = "Response body required but missing";
        return false;
      }

      if (response.contains("body")) {
        auto &body = response["body"];
        if (body.contains("response")) {
          const std::string &body_str = body["response"];
          if (body_str.length() > m_max_body_size) {
            error = std::format("Response body too large: {} > {}",
                                body_str.length(), m_max_body_size);
            return false;
          }
        }
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
