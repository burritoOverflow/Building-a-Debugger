#ifndef SDB_TYPES_HPP
#define SDB_TYPES_HPP

#include <array>

namespace sdb {
  using byte64  = std::array<std::byte, 8>;
  using byte128 = std::array<std::byte, 16>;

  class VirtualAddress {
public:
    VirtualAddress() = default;
    std::uint64_t GetAddress() const { return address_; }

    explicit VirtualAddress(const std::uint64_t address) : address_(address) {}

    VirtualAddress operator+(const std::uint64_t offset) const {
      return VirtualAddress(address_ + offset);
    }

    VirtualAddress operator-(const std::uint64_t offset) const {
      return VirtualAddress(address_ - offset);
    }

    VirtualAddress &operator+=(const std::uint64_t offset) {
      address_ += offset;
      return *this;
    }

    VirtualAddress &operator-=(const std::uint64_t offset) {
      address_ -= offset;
      return *this;
    }

    bool operator==(const VirtualAddress &other) const {
      return address_ == other.address_;
    }

    bool operator!=(const VirtualAddress &other) const {
      return address_ != other.address_;
    }

    bool operator<(const VirtualAddress &other) const {
      return address_ < other.address_;
    }

    bool operator<=(const VirtualAddress &other) const {
      return address_ <= other.address_;
    }

    bool operator>(const VirtualAddress &other) const {
      return address_ > other.address_;
    }

    bool operator>=(const VirtualAddress &other) const {
      return address_ >= other.address_;
    }

private:
    std::uint64_t address_ = 0;
  };
}  // namespace sdb

#endif  // SDB_TYPES_HPP
