#ifndef SDB_BIT_HPP
#define SDB_BIT_HPP

#include <cstddef>
#include <cstring>
#include <libsdb/types.hpp>
#include <string_view>
#include <vector>

namespace sdb {
  template <class To>
  To FromBytes(const std::byte *bytes) {
    To ret;
    std::memcpy(&ret, bytes, sizeof(To));
    return ret;
  }

  template <class From>
  std::byte *AsBytes(From &from) {
    return reinterpret_cast<std::byte *>(&from);
  }

  template <class From>
  const std::byte *AsBytes(const From &from) {
    return reinterpret_cast<const std::byte *>(&from);
  }


  template <class From>
  byte128 ToByte128(const From &src) {
    // initialize the byte128 array to 0;
    // we rely on this behavior elsewhere
    byte128 ret{};
    std::memcpy(&ret, &src, sizeof(From));
    return ret;
  }

  template <class From>
  byte64 ToByte64(const From &src) {
    byte64 ret{};
    std::memcpy(&ret, &src, sizeof(From));
    return ret;
  }

  inline std::string_view ToStringView(const std::byte  *data,
                                       const std::size_t size) {
    return {reinterpret_cast<const char *>(data), size};
  }

  inline std::string_view ToStringView(const std::vector<std::byte> &data) {
    return ToStringView(data.data(), data.size());
  }
}  // namespace sdb

#endif  // SDB_BIT_HPP
