#ifndef INTERFACES_HPP
#define INTERFACES_HPP

#include <memory>
#include <string>

class IConnectableClient {
public:
  virtual ~IConnectableClient() = default;
  virtual bool is_connected() const = 0;
};

class ITracer {
public:
  virtual ~ITracer() = default;
  virtual void log_request(const std::string &method, const std::string &url,
                           int status_code, long long start_us,
                           long long end_us, const std::string &service_name,
                           const std::string &request_id,
                           const std::string &trace_id,
                           const std::string &span_id,
                           const std::string &parent_id,
                           const std::string &additional_attributes_json) = 0;
};

#endif // INTERFACES_HPP