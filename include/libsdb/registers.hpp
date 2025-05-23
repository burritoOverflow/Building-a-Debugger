#ifndef SDB_REGISTERS_HPP
#define SDB_REGISTERS_HPP

#include <libsdb/register_info.hpp>
#include <libsdb/types.hpp>
#include <sys/user.h>
#include <variant>

namespace sdb {
  class Process;

  class Registers {
public:
    Registers() = delete;
    // Register instances should be unique, so disallow copying and default
    // construction
    Registers(const Registers &)            = delete;
    Registers &operator=(const Registers &) = delete;

    using value =
        std::variant<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t,
                     std::int8_t, std::int16_t, std::int32_t, std::int64_t,
                     float, double, long double, byte64, byte128>;

    value Read(const RegisterInfo &info) const;
    void  Write(const RegisterInfo &info, value value);

    template <class T>
    T ReadByIdAs(const RegisterID id) const {
      return std::get<T>(this->Read(RegisterInfoByID(id)));
    }

    void WriteById(const RegisterID id, const value &val) {
      this->Write(RegisterInfoByID(id), val);
    }

private:
    friend Process;  // Process should be able to construct a Registers object
    explicit Registers(Process &proc) : proc_(&proc) {}

    user     data_;
    Process *proc_;  // pointer to our parent process to allow it to read mem
                     // for us
  };

}  // namespace sdb

#endif  // SDB_REGISTERS_HPP
