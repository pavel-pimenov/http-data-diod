#ifndef ERROR_TYPES_HPP
#define ERROR_TYPES_HPP

#include <cstdint>
#include <prometheus/counter.h>
#include <string>

// ============================================================================
// Error Type Enumerations
// ============================================================================

enum class HttpErrorType : uint8_t {
  CONNECTION_ERROR,
  SSL_ERROR,
  TIMEOUT_ERROR,
  STATUS_CODE_ERROR,
  PROTOCOL_ERROR,
  RATE_LIMIT_ERROR,
  SERVER_ERROR,
  CLIENT_ERROR,
  OTHER_ERROR
};

enum class L2ErrorType : uint8_t {
  CONNECTION_ERROR,
  TIMEOUT_ERROR,
  RESPONSE_ERROR,
  RETRY_EXHAUSTED,
  OTHER_ERROR
};

enum class ProcessingErrorType : uint8_t {
  JSON_PARSE_ERROR,
  VALIDATION_ERROR,
  DECOMPRESSION_ERROR,
  ENCODING_ERROR,
  TIMEOUT_ERROR,
  RESOURCE_EXHAUSTED,
  OTHER_ERROR
};

// ============================================================================
// Error Metrics Structs
// ============================================================================

struct L2ErrorMetrics {
  prometheus::Counter *m_total_errors = nullptr;
  prometheus::Counter *m_connection_errors = nullptr;
  prometheus::Counter *m_timeout_errors = nullptr;
  prometheus::Counter *m_other_errors = nullptr;
};

struct ProcessingErrorMetrics {
  prometheus::Counter *m_total_errors = nullptr;
  prometheus::Counter *m_json_errors = nullptr;
  prometheus::Counter *m_validation_errors = nullptr;
  prometheus::Counter *m_decompression_errors = nullptr;
  prometheus::Counter *m_other_errors = nullptr;
};

// ============================================================================
// Error Categorization
// ============================================================================

HttpErrorType categorize_http_error(const std::string &error_msg,
                                    int status_code = 0);
L2ErrorType categorize_l2_error(const std::string &error_msg);
ProcessingErrorType categorize_processing_error(const std::string &error_msg);

std::string http_error_type_to_string(HttpErrorType type);
std::string l2_error_type_to_string(L2ErrorType type);
std::string processing_error_type_to_string(ProcessingErrorType type);

// ============================================================================
// Error Handling Functions
// ============================================================================

void handle_error(const std::string &error_msg,
                  prometheus::Counter *metrics_counter = nullptr,
                  bool log_error = true);
void handle_exception(const std::exception &e,
                      prometheus::Counter *metrics_counter = nullptr,
                      const std::string &prefix_msg = "");
void handle_http_error(const std::string &error_msg,
                       prometheus::Counter *metrics_counter = nullptr,
                       const std::string &operation = "", int attempt = 0,
                       const std::string &url = "");

void handle_l2_error_with_category(const std::string &error_msg,
                                   const L2ErrorMetrics &metrics,
                                   const std::string &operation = "");
void handle_processing_error_with_category(
    const std::string &error_msg, const ProcessingErrorMetrics &metrics,
    const std::string &operation = "");

#endif // ERROR_TYPES_HPP
