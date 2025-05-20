#ifndef SDB_REGISTER_INFO_HPP
#define SDB_REGISTER_INFO_HPP

#include <algorithm>
#include <libsdb/error.hpp>
#include <string_view>
#include <sys/user.h>

namespace sdb {
  enum class RegisterID {
#define DEFINE_REGISTER(name, dwarf_id, size, offset, type, format) name
#include <libsdb/detail/registers.inc>
#undef DEFINE_REGISTER
  };

  /*
   * General Purpose, Sub General Purpose (sub-register of a GPR),
   * Floating Point, Debug Registers
   */
  enum class RegisterType { GPR, SUB_GPR, FPR, DR };

  // different ways to interpret the register
  enum class RegisterFormat { UINT, DOUBLE_FLOAT, LONG_DOUBLE, VECTOR };

  struct RegisterInfo {
    RegisterID       id;
    std::string_view name;
    std::int32_t     dwarf_id;
    std::size_t      size;
    std::size_t      offset;
    RegisterType     type;
    RegisterFormat   format;
  };

  // information for every register in the system
  inline constexpr const RegisterInfo gRegisterInfos[] = {
#define DEFINE_REGISTER(name, dwarf_id, size, offset, type, format) \
  {RegisterID::name, #name, dwarf_id, size, offset, type, format}
#include <libsdb/detail/registers.inc>
#undef DEFINE_REGISTER
  };

  template <class F>
  const RegisterInfo &RegisterInfoBy(F f) {
    const auto it =
        std::find_if(std::begin(gRegisterInfos), std::end(gRegisterInfos), f);
    if (it == std::end(gRegisterInfos)) {
      Error::Send("Can't find register info");
    }
    return *it;
  }

  inline const RegisterInfo &RegisterInfoByID(const RegisterID id) {
    return RegisterInfoBy([id](auto &i) { return i.id == id; });
  }

  inline const RegisterInfo &RegisterInfoByName(const std::string_view name) {
    return RegisterInfoBy([name](auto &i) { return i.name == name; });
  }

  inline const RegisterInfo &RegisterInfoByDwarfID(
      const std::int32_t dwarf_id) {
    return RegisterInfoBy([dwarf_id](auto &i)
                          { return i.dwarf_id == dwarf_id; });
  }

}  // namespace sdb

#endif  // SDB_REGISTER_INFO_HPP
