#ifndef SDB_TYPES_HPP
#define SDB_TYPES_HPP

#include <array>

namespace sdb {
  using byte64 = std::array<std::byte, 8>;
  using byte128 = std::array<std::byte, 16>;
}

#endif // SDB_TYPES_HPP
