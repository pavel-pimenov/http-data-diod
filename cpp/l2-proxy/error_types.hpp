#ifndef ERROR_TYPES_HPP
#define ERROR_TYPES_HPP

#include "error_categorizer.hpp"
#include <prometheus/counter.h>

// ============================================================================
// Error Type Enumerations
// ============================================================================
//
// The error-type enums (HttpErrorType, L2ErrorType, ProcessingErrorType) live
// in error_categorizer.hpp (dependency-light, unit-testable without
// prometheus). error_types.hpp re-exports the categorizers and adds the
// prometheus-backed error-handling helpers below.

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
//
// The categorizers and enum-to-string helpers are implemented (header-only)
// in error_categorizer.hpp inside the error_categorizer namespace. Re-expose
// them and the error-type enums at global scope for backward compatibility
// with existing callers.

using error_categorizer::HttpErrorType;
using error_categorizer::L2ErrorType;
using error_categorizer::ProcessingErrorType;
using error_categorizer::categorize_http_error;
using error_categorizer::categorize_l2_error;
using error_categorizer::categorize_processing_error;
using error_categorizer::http_error_type_to_string;
using error_categorizer::l2_error_type_to_string;
using error_categorizer::processing_error_type_to_string;

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
