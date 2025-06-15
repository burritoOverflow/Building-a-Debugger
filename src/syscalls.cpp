#include <libsdb/error.hpp>
#include <libsdb/syscalls.hpp>
#include <unordered_map>

std::string_view sdb::SyscallIdToName(const int id) {
  switch (id) {
#define DEFINE_SYSCALL(name, id) \
  case id:                       \
    return #name;
#include "include/syscalls.inc"
#undef DEFINE_SYSCALL
    default:
      Error::Send("No such syscall");
  }
}

namespace {
  const std::unordered_map<std::string_view, int> g_syscall_name_map = {
#define DEFINE_SYSCALL(name, id) {#name, id},
#include "include/syscalls.inc"
#undef DEFINE_SYSCALL
  };
}  // namespace

int sdb::SyscallNameToId(const std::string_view name) {
  if (g_syscall_name_map.count(name) != 1) {
    Error::Send("No such syscall");
  }
  return g_syscall_name_map.at(name);
}
