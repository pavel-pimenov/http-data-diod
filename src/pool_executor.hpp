#ifndef POOL_EXECUTOR_HPP
#define POOL_EXECUTOR_HPP

#include "logger.hpp"
#include <stdexcept>
#include <utility>

template <typename HttpClientPoolType, typename Func>
inline auto execute_http_command_with_status(HttpClientPoolType *pool,
                                             Func &&func)
    -> std::pair<decltype(func(std::declval<
                               typename HttpClientPoolType::client_type *>())),
                 int> {
  using ReturnType = decltype(func(
      std::declval<typename HttpClientPoolType::client_type *>()));

  if (!pool) {
    throw std::runtime_error("HTTP pool is null");
  }

  auto http_client = pool->acquire_connection();
  if (!http_client) {
    throw std::runtime_error(
        "Failed to acquire valid HTTP connection - connection is null");
  }
  if (!http_client->is_valid()) {
    pool->release_connection(std::move(http_client));
    throw std::runtime_error(
        "Failed to acquire valid HTTP connection - connection is invalid");
  }

  try {
    ReturnType result = std::forward<Func>(func)(http_client.get());
    int status_code = http_client->get_last_status_code();
    pool->release_connection(std::move(http_client));
    return std::make_pair(result, status_code);
  } catch (...) {
    http_client->invalidate();
    pool->release_connection(std::move(http_client));
    throw;
  }
}

#endif // POOL_EXECUTOR_HPP
