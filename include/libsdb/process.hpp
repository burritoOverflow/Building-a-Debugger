#ifndef SDB_PROCESS_HPP
#define SDB_PROCESS_HPP

#include <filesystem>
#include <libsdb/registers.hpp>
#include <memory>
#include <optional>

namespace sdb {
  // current running state of the process
  enum class ProcessState {
    Running,
    Stopped,
    Exited,
    Terminated,
  };

  struct StopReason {
    explicit StopReason(int wait_status);

    ProcessState reason;
    std::uint8_t info;
  };

  class Process {
public:
    Process()                           = delete;
    Process(Process &)                  = delete;
    Process &operator=(const Process &) = delete;
    ~Process();

    Registers       &GetRegisters() { return *this->registers_; }
    const Registers &GetRegisters() const { return *this->registers_; }

    void WriteUserArea(std::size_t offset, std::uint64_t data) const;
    void WriteFprs(const user_fpregs_struct &fprs) const;
    void WriteGprs(const user_regs_struct &gprs) const;

    // path to the program to launch
    static std::unique_ptr<Process> Launch(
        const std::filesystem::path &program_path, bool debug = true,
        std::optional<int> stdout_replacement = std::nullopt);

    // takes a PID of an existing process to attach to
    static std::unique_ptr<Process> Attach(pid_t pid);

    // Resume a currently halted process
    void Resume();

    StopReason WaitOnSignal();

    pid_t        pid() const { return pid_; }
    ProcessState state() const { return state_; }

private:
    // for static members to construct a Process object
    Process(const pid_t pid, const bool terminate_on_end,
            const bool is_attached) :
        pid_(pid), terminate_on_end_(terminate_on_end),
        is_attached_(is_attached), registers_(new Registers(*this)) {}

    void ReadAllRegisters();

    // for the process we're tracking
    pid_t pid_ = 0;
    // should we terminate the process?
    bool terminate_on_end_ = true;
    bool is_attached_      = false;
    // current state of the process
    ProcessState state_ = ProcessState::Stopped;

    std::unique_ptr<Registers> registers_;
  };
}  // namespace sdb

#endif  // SDB_PROCESS_HPP
