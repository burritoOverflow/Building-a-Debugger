#include <filesystem>
#include <libsdb/elf.hpp>
#include <libsdb/process.hpp>
#include <libsdb/target.hpp>
#include <memory>

namespace {
  std::unique_ptr<sdb::Elf> CreateLoadedElf(const sdb::Process&         process,
                                            const std::filesystem::path& path) {
    auto auxv = process.GetAuxiliaryVector();
    auto obj  = std::make_unique<sdb::Elf>(path);
    // sets the load address of Elf ptr's .text section by subtracting the load
    // address of the entry point in the ELF file from the actual load address
    // of the entry point
    obj->NotifyLoaded(
        sdb::VirtualAddress(auxv[AT_ENTRY] - obj->GetHeader().e_entry));
    return obj;
  }
}  // namespace

std::unique_ptr<sdb::Target> sdb::Target::Launch(
    const std::filesystem::path& path,
    const std::optional<int>     stdout_replacement) {
  auto proc = Process::Launch(path, true, stdout_replacement);
  auto obj  = CreateLoadedElf(*proc, path);
  return std::unique_ptr<Target>(new Target(std::move(proc), std::move(obj)));
}

std::unique_ptr<sdb::Target> sdb::Target::Attach(const pid_t pid) {
  // /proc/<pid>/exe is a symlink to the executable of the process
  auto elf_path = std::filesystem::path("/proc") / std::to_string(pid) / "exe";
  auto proc     = Process::Attach(pid);
  auto obj      = CreateLoadedElf(*proc, elf_path);
  return std::unique_ptr<Target>(new Target(std::move(proc), std::move(obj)));
}
