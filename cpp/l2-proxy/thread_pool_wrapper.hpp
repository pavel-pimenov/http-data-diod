#ifndef THREAD_POOL_WRAPPER_HPP
#define THREAD_POOL_WRAPPER_HPP

#include "thread_pool.hpp"
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>

class ThreadPoolWrapper {
public:
  enum class Type : uint8_t { CUSTOM, NONE };

  ThreadPoolWrapper(Type type, size_t num_threads, size_t max_queue_size = 0)
      : m_type(type) {
    switch (m_type) {
    case Type::CUSTOM:
      m_custom_pool = std::make_unique<ThreadPool>(num_threads, max_queue_size);
      break;
    case Type::NONE:
      break;
    }
  }

  template <class F, class... Args>
  auto enqueue(F &&f, Args &&...args)
      -> std::future<std::invoke_result_t<F, Args...>> {

    switch (m_type) {
    case Type::CUSTOM:
      return m_custom_pool->enqueue(std::forward<F>(f),
                                    std::forward<Args>(args)...);
    case Type::NONE: {
      auto task = std::make_shared<
          std::packaged_task<std::invoke_result_t<F, Args...>()>>(
          [f = std::forward<F>(f),
           ... args = std::forward<Args>(args)]() mutable {
            return std::invoke(f, args...);
          });
      std::future<std::invoke_result_t<F, Args...>> result = task->get_future();
      (*task)();
      return result;
    }
    default:
      throw std::runtime_error("Unknown thread pool type");
    }
  }

  [[nodiscard]] size_t queue_size() const {
    return m_custom_pool ? m_custom_pool->queue_size() : 0;
  }

private:
  Type m_type;
  std::unique_ptr<ThreadPool> m_custom_pool;
};

#endif // THREAD_POOL_WRAPPER_HPP
