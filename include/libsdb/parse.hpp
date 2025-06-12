#ifndef SDB_PARSE_HPP
#define SDB_PARSE_HPP

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace sdb {
  template <class I>
  std::optional<I> ToIntegral(std::string_view sv, int base = 10) {
    auto begin = sv.begin();
    // skip past the leading '0x' for hexadecimal if present
    if (base == 16 && sv.size() > 1 && begin[0] == '0' && begin[1] == 'x') {
      begin += 2;
    }

    I ret;
    if (auto result = std::from_chars(begin, sv.end(), ret, base);
        result.ptr != sv.end()) {
      // return empty if some input remains
      return std::nullopt;
    }
    return ret;
  }

  // called only when std::byte is specified as the template argument
  template <>
  inline std::optional<std::byte> ToIntegral(const std::string_view sv,
                                             const int              base) {
    const auto uint8t = ToIntegral<std::uint8_t>(sv, base);
    if (!uint8t) {
      return std::nullopt;
    }
    return static_cast<std::byte>(*uint8t);
  }

  template <class F>
  std::optional<F> ToFloat(std::string_view sv) {
    F          ret;
    const auto result = std::from_chars(sv.begin(), sv.end(), ret);
    if (result.ptr != sv.end()) {
      return std::nullopt;
    }
    return ret;
  }

  template <std::size_t N>
  auto ParseVector(const std::string_view text) {
    // signal there was an error
    auto invalid = [] { sdb::Error::Send("Invalid format"); };

    std::array<std::byte, N> bytes;
    const char*              c = text.data();

    // check for the leading bracket
    if (*c++ != '[') {
      invalid();
    }

    for (auto i = 0; i < N - 1; ++i) {
      bytes[i] = ToIntegral<std::byte>({c, 4}, 16).value();
      c += 4;
      // check if there's a comma present
      if (*c++ != ',') {
        invalid();
      }
    }

    bytes[N - 1] = ToIntegral<std::byte>({c, 4}, 16).value();
    c += 4;

    // check for the closing bracket
    if (*c != ']') {
      invalid();
    }
    // check the end of the string
    if (c != text.end()) {
      invalid();
    }

    return bytes;
  }

  /*
   * we expect input in the form of hexadecimal values, comma-separated and
   * surrounded in square brackets, as in 'mem write 0x555555555156 [0xff,0xff]'
   */
  inline auto ParseVector(const std::string_view text) {
    auto invalid = [] { sdb::Error::Send("Invalid format"); };

    std::vector<std::byte> bytes;
    const char*            c = text.data();

    if (*c++ != '[') {
      invalid();
    }

    while (*c != ']') {
      // parse 4 characters as a byte (e.g. '0xff')
      auto byte = ToIntegral<std::byte>({c, 4}, 16);
      bytes.push_back(byte.value());
      // move past this value
      c += 4;

      // either a comma or the closing bracket, for the next byte or the end of
      // the values, respectively
      if (*c == ',') {
        ++c;
      } else if (*c != ']') {
        invalid();
      }
    }

    if (++c != text.end()) {
      invalid();
    }

    return bytes;
  }
}  // namespace sdb

#endif  // SDB_PARSE_HPP
