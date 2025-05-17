#ifndef SDB_ERROR_HPP
#define SDB_ERROR_HPP

#include <cstring>
#include <stdexcept>

namespace sdb {
  class Error final : public std::runtime_error {
public:
    [[noreturn]] static void Send(const std::string &what) {
      throw Error(what);
    }

    [[noreturn]] static void SendErrno(const std::string &prefix) {
      throw Error(prefix + ": " + std::strerror(errno));
    }

private:
    explicit Error(const std::string &what) : std::runtime_error(what) {}
  };
}  // namespace sdb

#endif  // SDB_ERROR_HPP
