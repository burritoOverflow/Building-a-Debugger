#ifndef SDB_TYPES_HPP
#define SDB_TYPES_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace sdb {
  using byte64  = std::array<std::byte, 8>;
  using byte128 = std::array<std::byte, 16>;

  enum class StoppointMode { write, read_write, execute };

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

  // represents a view of an existing region of memory
  template <class T>
  class Span {
public:
    Span() = default;

    // start and size
    Span(T *data, const std::size_t size) : data_(data), size_(size) {}

    // start and end ptrs
    Span(T *data, T *end) : data_(data), size_(end - data) {}

    template <class U>
    explicit Span(const std::vector<U> &vec) :
        data_(vec.data()), size_(vec.size()) {}

    T          *begin() { return data_; }
    T          *end() { return data_ + size_; }
    std::size_t Size() const { return size_; }
    T          &operator[](std::size_t n) { return *(data_ + n); }

private:
    T          *data_;
    std::size_t size_ = 0;
  };
}  // namespace sdb

#endif  // SDB_TYPES_HPP
