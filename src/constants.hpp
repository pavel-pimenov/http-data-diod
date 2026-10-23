#ifndef CONSTANTS_HPP
#define CONSTANTS_HPP

#include <cstddef>

// Cross-cutting protocol/port constants. Values that belong to a single
// module stay next to that module; only constants shared by several
// translation units live here.

namespace http_status {
inline constexpr int kOk = 200;
inline constexpr int kBadRequest = 400;
inline constexpr int kForbidden = 403;
inline constexpr int kNotFound = 404;
inline constexpr int kMethodNotAllowed = 405;
inline constexpr int kConflict = 409;
inline constexpr int kUnprocessableEntity = 422;
inline constexpr int kTooManyRequests = 429;
inline constexpr int kInternalServerError = 500;
inline constexpr int kBadGateway = 502;
inline constexpr int kServiceUnavailable = 503;
inline constexpr int kGatewayTimeout = 504;

inline constexpr int kSuccessMin = 200;   // 2xx
inline constexpr int kSuccessMax = 300;   // exclusive upper bound of 2xx
inline constexpr int kRedirectMax = 400;  // exclusive upper bound of 3xx
inline constexpr int kClientErrorMin = 400;
inline constexpr int kServerErrorMin = 500;
} // namespace http_status

namespace service_port {
inline constexpr int kProxyMetrics = 19090;
inline constexpr int kWorkerMetrics = 19091;
inline constexpr int kServerMetrics = 19092;
inline constexpr int kWorkerHealth = 19093;
} // namespace service_port

namespace http_server_limits {
inline constexpr int kKeepAliveMaxCount = 100;
inline constexpr int kKeepAliveTimeoutSeconds = 5;
inline constexpr std::size_t kMaxRequestPayloadBytes =
    std::size_t{10} * 1024 * 1024; // 10 MB, large enough for batched DB requests
} // namespace http_server_limits

namespace net_limits {
inline constexpr int kMinTcpPort = 1;
inline constexpr int kMaxTcpPort = 65535;
inline constexpr int kHttpPort = 80;
inline constexpr int kHttpsPort = 443;
// Above this the connection pool mostly wastes file descriptors: operators get
// a warning (not an error) at startup.
inline constexpr int kRecommendedHttpPoolSize = 1000;
} // namespace net_limits

namespace http_timeouts {
inline constexpr int kConnectTimeoutSeconds = 5;
inline constexpr int kPoolAcquireTimeoutSeconds = 30;
inline constexpr int kGracefulShutdownSeconds = 30;
inline constexpr int kShutdownPollIntervalMs = 100;
} // namespace http_timeouts

#endif // CONSTANTS_HPP
