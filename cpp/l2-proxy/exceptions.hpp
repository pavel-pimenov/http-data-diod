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

class NatsException : public L2ProxyException {
public:
  explicit NatsException(const std::string &msg)
      : L2ProxyException("NATS error: " + msg) {}
};

class L2ServerException : public L2ProxyException {
public:
  explicit L2ServerException(const std::string &msg)
      : L2ProxyException("L2 server error: " + msg) {}
};

class JsonException : public L2ProxyException {
public:
  explicit JsonException(const std::string &msg)
      : L2ProxyException("JSON error: " + msg) {}
};

class ConfigException : public L2ProxyException {
public:
  explicit ConfigException(const std::string &msg)
      : L2ProxyException("Config error: " + msg) {}
};

#endif // EXCEPTIONS_HPP