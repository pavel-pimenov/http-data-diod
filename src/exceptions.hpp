#ifndef EXCEPTIONS_HPP
#define EXCEPTIONS_HPP

#include <stdexcept>
#include <string>

class L2ProxyException : public std::runtime_error {
public:
  explicit L2ProxyException(const std::string &msg) : std::runtime_error(msg) {}
};

class TimeoutException : public L2ProxyException {
public:
  explicit TimeoutException(const std::string &msg)
      : L2ProxyException("Timeout error: " + msg) {}
};

#endif // EXCEPTIONS_HPP