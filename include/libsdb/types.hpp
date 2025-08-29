#ifndef SDB_TYPES_HPP
#define SDB_TYPES_HPP

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace sdb {
  class FileAddress;
  class Elf;

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

    // convert the virtual address to a FileAddress for a given ELF file
    FileAddress ToFileAddress(const Elf &obj) const;

private:
    std::uint64_t address_ = 0;
  };

  /*
   * Represents virtual addresses specified in the ELF file.
   */
  class FileAddress {
public:
    FileAddress() = default;
    FileAddress(const Elf &obj, const uint64_t &addr) :
        elf_(&obj), addr_(addr) {}

    std::uint64_t GetAddress() const { return addr_; }
    const Elf    *ElfFile() const { return elf_; }

    FileAddress operator+(const std::uint64_t offset) const {
      return FileAddress(*elf_, addr_ + offset);
    }

    FileAddress operator-(const std::uint64_t offset) const {
      return FileAddress(*elf_, addr_ - offset);
    }

    FileAddress &operator+=(const std::uint64_t offset) {
      addr_ += offset;
      return *this;
    }

    FileAddress &operator-=(const std::uint64_t offset) {
      addr_ -= offset;
      return *this;
    }

    bool operator==(const FileAddress &other) const {
      return addr_ == other.addr_ and elf_ == other.elf_;
    }

    bool operator!=(const FileAddress &other) const {
      return addr_ != other.addr_ or elf_ != other.elf_;
    }

    // NOTE: in the following, all ELF pointers must be equal; relative
    // comparison operators don't make sense if the ELF files for the address do
    // not match.
    bool operator<(const FileAddress &other) const {
      assert(elf_ == other.elf_);
      return addr_ < other.addr_;
    }

    bool operator<=(const FileAddress &other) const {
      assert(elf_ == other.elf_);
      return addr_ <= other.addr_;
    }

    bool operator>(const FileAddress &other) const {
      assert(elf_ == other.elf_);
      return addr_ > other.addr_;
    }

    bool operator>=(const FileAddress &other) const {
      assert(elf_ == other.elf_);
      return addr_ >= other.addr_;
    }

    VirtualAddress ToVirtualAddress(const Elf &obj) const;

private:
    const Elf    *elf_ = nullptr;
    std::uint64_t addr_ =
        0;  // this address is relative to the load address of this file
  };

  /*
   * Stores an offset instead of an address
   *
   * Represents absolute offsets from the start of the object file
   */
  class FileOffset {
public:
    FileOffset() = default;
    FileOffset(const Elf &obj, const uint64_t &offset) :
        elf_(&obj), offset_(offset) {}

    std::uint64_t GetOffset() const { return offset_; }
    const Elf    *ElfFile() const { return elf_; }

private:
    const Elf    *elf_    = nullptr;
    std::uint64_t offset_ = 0;
  };

  // represents a view to an existing region of memory
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

    T          *begin() const { return data_; }
    T          *end() const { return data_ + size_; }
    std::size_t Size() const { return size_; }
    T          &operator[](std::size_t n) { return *(data_ + n); }

private:
    T          *data_;
    std::size_t size_ = 0;
  };
}  // namespace sdb

#endif  // SDB_TYPES_HPP
