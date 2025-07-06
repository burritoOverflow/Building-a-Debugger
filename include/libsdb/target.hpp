#ifndef SDB_TARGET_HPP
#define SDB_TARGET_HPP

#include <filesystem>
#include <libsdb/process.hpp>
#include <memory>
#include <optional>

namespace sdb {
  class Target {
public:
    Target()                         = delete;
    Target(const Target&)            = delete;
    Target& operator=(const Target&) = delete;

    static std::unique_ptr<Target> Launch(
        const std::filesystem::path& path,
        std::optional<int>           stdout_replacement = std::nullopt);

    static std::unique_ptr<Target> Attach(pid_t pid);

    Process&       GetProcess() { return *this->process_; }
    const Process& GetProcess() const { return *this->process_; }

    Elf&       GetElf() { return *this->elf_; }
    const Elf& GetElf() const { return *this->elf_; }

private:
    Target(std::unique_ptr<Process> process, std::unique_ptr<Elf> elf) :
        process_(std::move(process)), elf_(std::move(elf)) {}

    std::unique_ptr<Process> process_;
    std::unique_ptr<Elf>     elf_;
  };
}  // namespace sdb

#endif  // SDB_TARGET_HPP
