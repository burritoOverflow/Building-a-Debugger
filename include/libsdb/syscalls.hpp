#ifndef SDB_SYSCALLS_HPP
#define SDB_SYSCALLS_HPP

#include <string_view>

namespace sdb {
  std::string_view SyscallIdToName(int id);
  int              SyscallNameToId(std::string_view name);
}  // namespace sdb

#endif  // SDB_SYSCALLS_HPP
