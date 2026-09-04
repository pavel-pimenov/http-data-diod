#ifndef INTERFACES_HPP
#define INTERFACES_HPP

#include <memory>
#include <string>

class IConnectableClient {
public:
  virtual ~IConnectableClient() = default;
  virtual bool is_connected() const = 0;
};

#endif // INTERFACES_HPP