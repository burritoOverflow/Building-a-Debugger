#ifndef SDB_PROCESS_HPP
#define SDB_PROCESS_HPP

#include <filesystem>
#include <libsdb/bit.hpp>
#include <libsdb/breakpoint_site.hpp>
#include <libsdb/registers.hpp>
#include <libsdb/stoppoint_collection.hpp>
#include <libsdb/watchpoint.hpp>
#include <memory>
#include <optional>

namespace sdb {
  class SyscallCatchPolicy {
public:
    enum Mode { None, Some, All };

    static SyscallCatchPolicy CatchAll() {
      return SyscallCatchPolicy{Mode::All, {}};
    }

    static SyscallCatchPolicy CatchNone() {
      return SyscallCatchPolicy{Mode::None, {}};
    }

    static SyscallCatchPolicy CatchSome(std::vector<int> to_catch) {
      return SyscallCatchPolicy{Mode::Some, std::move(to_catch)};
    }

    Mode GetMode() const { return mode_; }

    const std::vector<int> &GetToCatch() const { return to_catch_; }

private:
    SyscallCatchPolicy(Mode mode, std::vector<int> to_catch) :
        mode_(mode), to_catch_(std::move(to_catch)) {}

    Mode             mode_ = Mode::None;  // default to catching no syscalls
    std::vector<int> to_catch_;  // list of syscall IDs we should catch in the
                                 // case that `mode_` is set to 'Some'
  };

  // When PTRACE_SYSCALL is requested, the inferior will halt twice--once on
  // entry and once on exit of the syscall.
  // This type is used for checking the arguments to the syscall and the return
  // value
  struct SyscallInformation {
    std::uint16_t id;     // syscall number
    bool          entry;  // entry or exit of the syscall
    union {
      std::array<std::uint64_t, 6> args;  // arguments provided to the syscall
      std::uint64_t return_value;         // used when the syscall has exited
    };
  };

  // current running state of the process
  enum class ProcessState {
    Running,
    Stopped,
    Exited,
    Terminated,
  };

  // describes whether a SIGTRAP occurred due to single step, a software
  // breakpoint, hardware breakpoint, or an unknown reason
  enum class TrapType {
    SingleStep,
    SoftwareBreakpoint,
    HardwareBreakpoint,
    Syscall,
    Unknown,
  };

  struct StopReason {
    explicit StopReason(int wait_status);

    ProcessState                      reason;
    std::uint8_t                      info;
    std::optional<TrapType>           trap_reason;
    std::optional<SyscallInformation> syscall_info;
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

    StopReason StepInstruction();

    StopReason WaitOnSignal();

    pid_t        GetPid() const { return pid_; }
    ProcessState state() const { return state_; }

    VirtualAddress GetPc() const {
      return VirtualAddress{
          this->GetRegisters().ReadByIdAs<std::uint64_t>(RegisterID::rip)};
    }

    std::variant<BreakpointSite::id_type, Watchpoint::id_type>
    GetCurrentHardwareStoppoint() const;

    // Write the given address to the program counter (RIP)
    void SetPc(const VirtualAddress address) {
      this->GetRegisters().WriteById(RegisterID::rip, address.GetAddress());
    }

    // takes a virtual address to read from and the number of bytes to read
    // and returns the data as a std::vector<std::byte>
    std::vector<std::byte> ReadMemory(VirtualAddress address,
                                      std::size_t    amount) const;

    // read the contents of memory with all int3 instructions replaced with the
    // original byte
    std::vector<std::byte> ReadMemoryWithoutTraps(VirtualAddress address,
                                                  std::size_t    amount) const;

    template <class T>
    T ReadMemoryAs(const VirtualAddress address) const {
      auto data = this->ReadMemory(address, sizeof(T));
      return FromBytes<T>(data);
    }

    // takes a virtual address to write to and a Span<const std::byte>
    // representing the data to write
    void WriteMemory(VirtualAddress address, Span<const std::byte> data) const;

    // create a breakpoint site at the given address
    BreakpointSite &CreateBreakpointSite(VirtualAddress address,
                                         bool           hardware = false,
                                         bool           internal = false);

    Watchpoint &CreateWatchpoint(VirtualAddress address, StoppointMode mode,
                                 std::size_t size);

    StoppointCollection<Watchpoint> &GetWatchpoints() {
      return this->watchpoints_;
    }

    const StoppointCollection<Watchpoint> &GetWatchpoints() const {
      return this->watchpoints_;
    }

    int SetWatchpoint(Watchpoint::id_type id, VirtualAddress address,
                      StoppointMode mode, std::size_t size);

    int SetHardwareBreakpoint(BreakpointSite::id_type id,
                              VirtualAddress          address);

    StoppointCollection<BreakpointSite> &GetBreakpointSites() {
      return this->breakpoint_sites_;
    }

    const StoppointCollection<BreakpointSite> &GetBreakpointSites() const {
      return this->breakpoint_sites_;
    }

    void ClearHardwareStoppoint(int index);

    void SetSyscallCatchPolicy(SyscallCatchPolicy info) {
      this->syscall_catch_policy_ = std::move(info);
    }

private:
    // for static members to construct a
    // Process object
    Process(const pid_t pid, const bool terminate_on_end,
            const bool is_attached) :
        pid_(pid), terminate_on_end_(terminate_on_end),
        is_attached_(is_attached), registers_(new Registers(*this)) {}

    void ReadAllRegisters();

    // used for both hardware breakpoints and watchpoints
    int SetHardwareStoppoint(VirtualAddress address, StoppointMode mode,
                             std::size_t size);

    // called when we know that the process has stopped because of a signal,
    // rather than because of exit or termination
    void AugmentStopReason(StopReason &reason);

    StopReason MaybeResumeFromSyscall(const StopReason &reason);

    // for the process we're tracking
    pid_t pid_ = 0;
    // should we terminate the process?
    bool terminate_on_end_ = true;
    bool is_attached_      = false;

    bool expecting_syscall_exit_ =
        false;  // used to track if we expect a syscall exit

    // current state of the process
    ProcessState                        state_ = ProcessState::Stopped;
    std::unique_ptr<Registers>          registers_;
    StoppointCollection<BreakpointSite> breakpoint_sites_;
    StoppointCollection<Watchpoint>     watchpoints_;
    SyscallCatchPolicy syscall_catch_policy_ = SyscallCatchPolicy::CatchNone();
  };
}  // namespace sdb

#endif  // SDB_PROCESS_HPP
