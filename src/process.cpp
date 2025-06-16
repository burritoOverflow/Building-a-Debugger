#include <bits/types/struct_iovec.h>
#include <csignal>
#include <libsdb/bit.hpp>
#include <libsdb/error.hpp>
#include <libsdb/pipe.hpp>
#include <libsdb/process.hpp>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  void ExitWithPerror(const sdb::Pipe &channel, std::string const &prefix) {
    const auto message = prefix + ": " + std::strerror(errno);
    channel.Write(reinterpret_cast<const std::byte *>(message.data()),
                  message.size());
    exit(-1);
  }

  void SetPtraceOptions(const pid_t pid) {
    if (ptrace(PTRACE_SETOPTIONS, pid, nullptr, PTRACE_O_TRACESYSGOOD) == -1) {
      sdb::Error::SendErrno("Failed to set TRACESYSGOOD option");
    }
  }
}  // namespace


// Write the given data to the user area at the given offset
void sdb::Process::WriteUserArea(const std::size_t   offset,
                                 const std::uint64_t data) const {
  if (ptrace(PTRACE_POKEUSER, this->pid_, offset, data) == -1) {
    Error::SendErrno("Could not write to user area");
  }
}

void sdb::Process::WriteFprs(const user_fpregs_struct &fprs) const {
  if (ptrace(PTRACE_SETFPREGS, this->pid_, nullptr, &fprs) == -1) {
    Error::SendErrno("Could not write FPRs");
  }
}

void sdb::Process::WriteGprs(const user_regs_struct &gprs) const {
  if (ptrace(PTRACE_SETREGS, this->pid_, nullptr, &gprs) == -1) {
    Error::SendErrno("Could not write GPRs");
  }
}

sdb::StopReason::StopReason(const int wait_status) {
  // if a given status represents an exit event
  if (WIFEXITED(wait_status)) {
    this->reason = ProcessState::Exited;
    this->info   = WEXITSTATUS(wait_status);
    // Then check if the stop was due to termination or or a stop
  } else if (WIFSIGNALED(wait_status)) {
    this->reason = ProcessState::Terminated;
    this->info   = WTERMSIG(wait_status);  // extract the signal number
  } else if (WIFSTOPPED(wait_status)) {
    this->reason = ProcessState::Stopped;
    this->info   = WSTOPSIG(wait_status);  // as above
  }
}

sdb::Process::~Process() {
  if (this->pid_ != 0) {
    int status;

    if (this->is_attached_) {
      // For DETACH to work, the inferior process must be stopped
      // if currently running, we send SIGSTOP
      if (this->state_ == ProcessState::Running) {
        kill(this->pid_, SIGSTOP);
        // wait on the status
        waitpid(this->pid_, &status, 0);
      }

      // detach from the process and let it continue
      ptrace(PTRACE_DETACH, this->pid_, nullptr, nullptr);
      kill(this->pid_, SIGCONT);
    }

    // if we want to terminate the inferior process, do so now via SIGKILL
    // and wait for it to terminate
    if (this->terminate_on_end_) {
      kill(this->pid_, SIGKILL);
      waitpid(this->pid_, &status, 0);
    }
  }
}

std::unique_ptr<sdb::Process> sdb::Process::Launch(
    const std::filesystem::path &program_path, const bool debug,
    const std::optional<int> stdout_replacement) {
  // we want the pipe to be closed when we call execlp, so we
  // don't leave stale file descriptors
  Pipe channel(/*close_on_exec=*/true);

  pid_t pid = 0;
  if ((pid = fork()) == -1) {
  }

  // if we're in the child process, execute debugee
  if (pid == 0) {
    /* change the inferior's process group
     * If pid is specified as 0, the calling process’s process group ID is
     * changed. If pgid is specified as 0, then the process group ID of the
     * process specified by pid is made the same as its process ID.
     * (PGID to the same value as the PID)
     */
    if (setpgid(0, 0) == -1) {
      ExitWithPerror(channel, "Could not set pgid");
    }

    // disable address space randomization for the newly launched child process
    personality(ADDR_NO_RANDOMIZE);
    channel.CloseReadFd();  // we're not using the read end of the pipe

    if (stdout_replacement) {
      // replace stdout with the provided file descriptor
      // closes the 2nd fd arg and duplicates the 1st to the 2nd,
      // now, anything sent to stdout will be sent to the replacement fd
      if (dup2(*stdout_replacement, STDOUT_FILENO) == -1) {
        ExitWithPerror(channel, "stdout replacement failed");
      }
    }

    // attempt to attach to an existing process with the provided PID (only if
    // debugging is enabled)
    // indicate that this process is to be traced by the parent process
    if (debug and
        ptrace(PTRACE_TRACEME, pid, /*addr=*/nullptr, /*data=*/nullptr) == -1) {
      ExitWithPerror(channel, "Tracing failed");
    }

    if (execlp(program_path.c_str(), program_path.c_str(), nullptr) == -1) {
      ExitWithPerror(channel, "exec failed");
    }
  }

  channel.CloseWriteFd();  // Parent - we're not using the write end of the pipe
  auto data = channel.Read();
  channel.CloseReadFd();

  // if data is present on the read end of the pipe, we wait for the child
  // process to terminate and send an error message w/ the given message.
  // When there's an error in the child process, the parent process will throw.
  if (!data.empty()) {
    waitpid(pid, nullptr, 0);
    const auto chars = reinterpret_cast<char *>(data.data());
    Error::Send(std::string(chars, chars + data.size()));
  }

  std::unique_ptr<Process> process(
      new Process(pid, /*terminate_on_end=*/true, debug));

  if (debug) {
    process->WaitOnSignal();
    SetPtraceOptions(process->pid_);
  }
  return process;
}

std::unique_ptr<sdb::Process> sdb::Process::Attach(const pid_t pid) {
  if (pid == 0) {
    Error::Send("Invalid PID");
  }

  if (ptrace(PTRACE_ATTACH, pid, /*addr=*/nullptr, /*data=*/nullptr) == -1) {
    Error::SendErrno("Could not attach");
  }

  std::unique_ptr<Process> process(
      new Process(pid, /*terminate_on_end=*/false, /*is_attached=*/true));
  process->WaitOnSignal();
  SetPtraceOptions(process->GetPid());
  return process;
}

sdb::StopReason sdb::Process::StepInstruction() {
  std::optional<BreakpointSite *> to_reenable;
  if (auto pc = this->GetPc();
      this->breakpoint_sites_.EnabledStopPointAtAddress(pc)) {
    auto &bp = this->breakpoint_sites_.GetByAddress(pc);
    // disable the breakpoint so we can step over it
    bp.Disable();
    // store this breakpoint site so we can re-enable it later
    to_reenable = &bp;
  }

  // step over instruction and wait
  if (ptrace(PTRACE_SINGLESTEP, this->pid_, nullptr, nullptr) == -1) {
    Error::SendErrno("Could not single step");
  }

  const auto reason = this->WaitOnSignal();
  // re-enable if we disabled
  if (to_reenable) {
    to_reenable.value()->Enable();
  }

  return reason;
}

// Force the process to resume and update its tracked running state
void sdb::Process::Resume() {
  // if the process is currently stopped at a breakpoint, we should step over
  // the breakpoint
  if (const auto pc = this->GetPc();
      this->breakpoint_sites_.EnabledStopPointAtAddress(pc)) {
    auto &bp = this->breakpoint_sites_.GetByAddress(pc);
    bp.Disable();
    // execute a single instruction
    if (ptrace(PTRACE_SINGLESTEP, this->pid_, nullptr, nullptr) == -1) {
      Error::SendErrno("Failed to single step");
    }

    int wait_status;
    // wait until the inferior has executed the instruction and halted
    if (waitpid(this->pid_, &wait_status, 0) == -1) {
      Error::SendErrno("waitpid failed");
    }
    // then re-enable the breakpoint
    bp.Enable();
  }
  // if the syscall catch policy is set to
  // 'None', we just continue the process, otherwise,
  // we use PTRACE_SYSCALL to catch syscalls (in that
  // case, the inferior will trap whenever a syscall
  // is entered or exited)
  const auto request =
      this->syscall_catch_policy_.GetMode() == SyscallCatchPolicy::Mode::None
          ? PTRACE_CONT
          : PTRACE_SYSCALL;

  // and continue the process
  if (ptrace(request, this->pid_, nullptr, nullptr) == -1) {
    // exit if we can't resume the process
    Error::SendErrno("Could not resume");
  }

  this->state_ = ProcessState::Running;
}

sdb::StopReason sdb::Process::WaitOnSignal() {
  int wait_status = 0;
  if (constexpr int options = 0;
      waitpid(this->pid_, &wait_status, options) == -1) {
    Error::SendErrno("waitpid failed");
  }
  StopReason stop_reason(wait_status);
  this->state_ = stop_reason.reason;

  if (this->is_attached_ and this->state() == ProcessState::Stopped) {
    // if we're attached to the process, and it's stopped, we
    // read the registers setting the internal state of the `data_` member to
    // reflect the contents of the registers
    this->ReadAllRegisters();
    this->AugmentStopReason(stop_reason);

    // if the process stopped due to SIGTRAP and the addr 1 byte below the PC
    // is an enabled breakpoint, we fix up the PC to point to the breakpoint
    const auto instruction_begin = this->GetPc() - 1;

    if (stop_reason.info == SIGTRAP) {
      // if a software breakpoint caused the stop, we walk the pc back 1 byte
      // to the start of the int3 instruction
      if (stop_reason.trap_reason == TrapType::SoftwareBreakpoint and
          this->breakpoint_sites_.ContainsAddress(instruction_begin) and
          this->breakpoint_sites_.GetByAddress(instruction_begin).IsEnabled()) {
        this->SetPc(instruction_begin);
        // if a hardware breakpoint caused the stop, and the stop point is a
        // watchpoint, we update the watchpoint's data
      } else if (stop_reason.trap_reason == TrapType::HardwareBreakpoint) {
        if (const auto id = this->GetCurrentHardwareStoppoint();
            id.index() == 1) {  // check the variant here (1 is watchpoint)
          // now, update data when a watchppoint triggers a stop
          this->watchpoints_.GetById(std::get<1>(id)).UpdateData();
        }
      } else if (stop_reason.trap_reason == TrapType::Syscall) {
        // replace the current stop reason with the one returned by
        // MaybeResumeFromSyscall
        stop_reason = this->MaybeResumeFromSyscall(stop_reason);
      }
    }
  }

  return stop_reason;
}

void sdb::Process::ReadAllRegisters() {
  // get GPR registers
  if (ptrace(PTRACE_GETREGS, this->pid_, nullptr,
             &this->GetRegisters().data_.regs) == -1) {
    Error::SendErrno("Could not read GPR registers");
  }

  // get FPR registers
  if (ptrace(PTRACE_GETFPREGS, this->pid_, nullptr,
             &this->GetRegisters().data_.i387) == -1) {
    Error::SendErrno("Could not read FPR registers");
  }

  // read the debug registers
  for (int i = 0; i < 8; ++i) {
    const auto id = static_cast<int>(RegisterID::dr0) + i;
    // get the register info
    const auto &info = RegisterInfoByID(static_cast<RegisterID>(id));

    errno = 0;
    const std::int64_t data =
        ptrace(PTRACE_PEEKUSER, this->pid_, info.offset, nullptr);

    if (errno != 0) {
      Error::SendErrno("Could not read debug register");
    }

    // write the retrieved data to the correct location in the
    // registers_ member.
    GetRegisters().data_.u_debugreg[i] = data;
  }
}

sdb::BreakpointSite &sdb::Process::CreateBreakpointSite(
    const VirtualAddress address, const bool hardware, const bool internal) {
  if (this->breakpoint_sites_.ContainsAddress(address)) {
    Error::Send("Breakpoint site already created at this address " +
                std::to_string(address.GetAddress()));
  }

  return this->breakpoint_sites_.Push(std::unique_ptr<BreakpointSite>(
      new BreakpointSite(*this, address, hardware, internal)));
}

sdb::Watchpoint &sdb::Process::CreateWatchpoint(const VirtualAddress address,
                                                const StoppointMode  mode,
                                                const std::size_t    size) {
  if (this->watchpoints_.ContainsAddress(address)) {
    Error::Send("Watchpoint already created at address " +
                std::to_string(address.GetAddress()));
  }

  return this->watchpoints_.Push(
      std::unique_ptr<Watchpoint>(new Watchpoint(*this, address, mode, size)));
}

int sdb::Process::SetWatchpoint([[maybe_unused]] Watchpoint::id_type id,
                                const VirtualAddress                 address,
                                const StoppointMode                  mode,
                                const std::size_t                    size) {
  return this->SetHardwareStoppoint(address, mode, size);
}

int sdb::Process::SetHardwareBreakpoint(
    [[maybe_unused]] BreakpointSite::id_type id, const VirtualAddress address) {
  // the size for execution-only hardware breakpoints is 1
  return this->SetHardwareStoppoint(address, StoppointMode::execute, 1);
}

std::variant<sdb::BreakpointSite::id_type, sdb::Watchpoint::id_type>
sdb::Process::GetCurrentHardwareStoppoint() const {
  auto      &regs   = this->GetRegisters();
  const auto status = regs.ReadByIdAs<std::uint64_t>(RegisterID::dr6);
  // find index of the least significant bit set in the status (count
  // trailing zeroes)
  const auto index = __builtin_ctzll(status);

  auto       id   = static_cast<int>(RegisterID::dr0) + index;
  const auto addr = VirtualAddress(
      regs.ReadByIdAs<std::uint64_t>(static_cast<RegisterID>(id)));

  using ret = std::variant<BreakpointSite::id_type, Watchpoint::id_type>;

  if (this->breakpoint_sites_.ContainsAddress(addr)) {
    auto site_id = this->breakpoint_sites_.GetByAddress(addr).GetId();
    return ret{std::in_place_index<0>, site_id};
  }

  auto watch_id = this->watchpoints_.GetByAddress(addr).GetId();
  return ret{std::in_place_index<1>, watch_id};
}

std::vector<std::byte> sdb::Process::ReadMemory(VirtualAddress address,
                                                std::size_t    amount) const {
  std::vector<std::byte> ret(amount);
  const iovec            local_desc{ret.data(), ret.size()};
  std::vector<iovec>     remote_descs;

  while (amount > 0) {
    // 0x1000 is the page size on x86_64 (4k), so we read up to the next page
    // (split the range of the data to be copied on page boundaries)
    auto       up_to_next_page = 0x1000 - (address.GetAddress() & 0xfff);
    const auto chunk_size      = std::min(amount, up_to_next_page);
    remote_descs.push_back(
        {reinterpret_cast<void *>(address.GetAddress()), chunk_size});
    amount -= chunk_size;
    address += chunk_size;
  }


  if (process_vm_readv(this->pid_, &local_desc, /*liovcnt=*/1,
                       remote_descs.data(), /*riovcnt=*/remote_descs.size(),
                       /*flags=*/0) == -1) {
    Error::SendErrno("Could not read process memory");
  }
  return ret;
}

std::vector<std::byte> sdb::Process::ReadMemoryWithoutTraps(
    const VirtualAddress address, const std::size_t amount) const {
  auto memory = this->ReadMemory(address, amount);

  const auto sites =
      this->breakpoint_sites_.GetInRegion(address, address + amount);

  for (const auto &site : sites) {
    // ignore hardware breakpoints and disabled sites
    if (!site->IsEnabled() || site->IsHardware()) {
      continue;  // skip disabled breakpoints
    }

    // for each enabled site, we replace the int3 instruction at the add on
    // which the breakpoint is set with the saved data.
    auto offset                 = site->Address() - address.GetAddress();
    memory[offset.GetAddress()] = site->saved_data_;  // restore the saved data
  }
  return memory;
}

void sdb::Process::WriteMemory(const VirtualAddress  address,
                               Span<const std::byte> data) const {
  std::size_t written = 0;

  // until we've written all the data provided by the caller
  while (written < data.Size()) {
    const auto remaining = data.Size() - written;

    // data to be written on this iteration
    std::uint64_t word;

    // if at least 8 bytes remain, write the next 8 bytes from the start of
    // the given buffer
    if (remaining >= 8) {
      word = FromBytes<std::uint64_t>(data.begin() + written);
    } else {
      // otherwise, we perform a partial memory write
      // read the 8 bytes we'll be writing to and cast a pointer
      auto       read      = ReadMemory(address + written, 8);
      const auto word_data = reinterpret_cast<char *>(&word);

      // copy the remaining data into the start of word_data
      std::memcpy(word_data, data.begin() + written, remaining);

      // followed by the bytes we're not trying to overwrite
      std::memcpy(word_data + remaining, read.data() + remaining,
                  8 - remaining);
    }

    // write the next 8 bytes to the inferior
    if (ptrace(PTRACE_POKEDATA, this->pid_, address + written, word) == -1) {
      Error::SendErrno("Failed to write memory");
    }

    // we've written 8 bytes.
    written += 8;
  }
}

namespace {
  std::uint64_t EncodeHardwareStoppointMode(const sdb::StoppointMode mode) {
    switch (mode) {
      case sdb::StoppointMode::write:
        return 0b01;  // write
      case sdb::StoppointMode::read_write:
        return 0b11;  // data read/write
      case sdb::StoppointMode::execute:
        return 0b00;  // instruction execution
      default:
        sdb::Error::Send("Invalid stoppoint mode");
    }
  }

  std::uint64_t EncodeHardwareStoppointSize(const std::size_t size) {
    switch (size) {
      case 1:
        return 0b00;
      case 2:
        return 0b01;
      case 4:
        return 0b11;
      case 8:
        return 0b10;
      default:
        sdb::Error::Send("Invalid stoppoint size");
    }
  }

  int FindFreeStoppointRegister(std::uint64_t control_register) {
    for (auto i = 0; i < 4; ++i) {
      // check the two enable bits in the control register that correspond to
      // each DR register until we find one that's not enabled
      if ((control_register & (0b11 << (i * 2))) == 0) {
        return i;
      }
    }
    sdb::Error::Send("No remaining hardware debug registers");
  }

}  // namespace

int sdb::Process::SetHardwareStoppoint(const VirtualAddress address,
                                       const StoppointMode  mode,
                                       const std::size_t    size) {
  auto &registers = this->GetRegisters();
  // read the debug control register
  const auto control = registers.ReadByIdAs<std::uint64_t>(RegisterID::dr7);

  // will return 0,1,2,3 depending on which register is free, or throw an
  // exception if there is no free space (so 'free_space' is effectively the
  // register number)
  const int free_space = FindFreeStoppointRegister(control);

  auto id = static_cast<int>(RegisterID::dr0) + free_space;
  // write the given address to the dr register corresponding to the free space
  registers.WriteById(static_cast<RegisterID>(id), address.GetAddress());

  const auto mode_flag = EncodeHardwareStoppointMode(mode);
  const auto size_flag = EncodeHardwareStoppointSize(size);

  const auto enable_bit = (1 << (free_space * 2));
  const auto mode_bits  = (mode_flag << (free_space * 4 + 16));
  const auto size_bits  = (size_flag << (free_space * 4 + 18));

  const auto clear_mask =
      (0b11 << (free_space * 2)) | (0b1111 << (free_space * 4 + 16));

  auto masked = control & ~clear_mask;

  masked |= enable_bit | mode_bits | size_bits;

  // write to the control register
  registers.WriteById(RegisterID::dr7, masked);

  return free_space;
}

void sdb::Process::AugmentStopReason(StopReason &reason) {
  siginfo_t siginfo;
  if (ptrace(PTRACE_GETSIGINFO, this->pid_, nullptr, &siginfo) == -1) {
    Error::SendErrno("Failed to get siginfo");
  }

  // check if syscall
  if (reason.info == (SIGTRAP | 0x80)) {
    auto       &sys_info = reason.syscall_info.emplace();
    const auto &regs     = this->GetRegisters();

    if (this->expecting_syscall_exit_) {  // syscall exit caused the stop
      sys_info.entry = false;
      sys_info.id    = regs.ReadByIdAs<std::uint64_t>(
          RegisterID::orig_rax);  // location of the syscall number
      sys_info.return_value = regs.ReadByIdAs<std::uint64_t>(
          RegisterID::rax);                   // location of the return value
      this->expecting_syscall_exit_ = false;  // the next syscall event will be
                                              // interpreted as an entry event
    } else {
      // handle entry
      sys_info.entry = true;
      sys_info.id =
          regs.ReadByIdAs<std::uint64_t>(RegisterID::orig_rax);  // as above

      // SYSV ABI arguments to syscall are in registers: rdi, rsi, rdx, r10, r8,
      // and r9, in that order.
      constexpr std::array<RegisterID, 6> args_registers = {
          RegisterID::rdi, RegisterID::rsi, RegisterID::rdx,
          RegisterID::r10, RegisterID::r8,  RegisterID::r9};

      for (auto i = 0; i < 6; ++i) {
        // read the syscall argument from the corresponding register
        sys_info.args[i] = regs.ReadByIdAs<std::uint64_t>(args_registers[i]);
      }

      // inverse of the above, we next expect a syscall exit
      this->expecting_syscall_exit_ = true;
    }

    reason.info        = SIGTRAP;
    reason.trap_reason = TrapType::Syscall;
    return;
  }

  this->expecting_syscall_exit_ = false;

  reason.trap_reason = TrapType::Unknown;
  if (reason.info == SIGTRAP) {
    switch (siginfo.si_code) {
      case TRAP_TRACE:
        reason.trap_reason = TrapType::SingleStep;
        break;
        /* Note: (verbatim from the book)
         *
         * The Linux kernel actually reports the
         * wrong values on x64: SI_KERNEL for software breakpoints and
         * TRAP_BRKPT for single-stepping over a syscall. Enough important tools
         * rely on this bug’s behavior that it’s just not worth fixing anymore.
         */
      case SI_KERNEL:
        reason.trap_reason = TrapType::SoftwareBreakpoint;
        break;
      case TRAP_HWBKPT:
        reason.trap_reason = TrapType::HardwareBreakpoint;
        break;
      default:;
    }
  }
}

sdb::StopReason sdb::Process::MaybeResumeFromSyscall(const StopReason &reason) {
  // we don't bother checking for Mode::None, as we don't trigger a stop in the
  // first place
  if (syscall_catch_policy_.GetMode() == SyscallCatchPolicy::Mode::Some) {
    const auto &to_catch = syscall_catch_policy_.GetToCatch();
    const auto  found =
        std::find(begin(to_catch), end(to_catch), reason.syscall_info->id);

    // not in the list, we resume the process and wait for the next signal
    if (found == to_catch.end()) {
      this->Resume();
      return this->WaitOnSignal();
    }
  }
  return reason;
}

void sdb::Process::ClearHardwareStoppoint(const int index) {
  const auto id = static_cast<int>(RegisterID::dr0) + index;
  this->GetRegisters().WriteById(static_cast<RegisterID>(id), 0);

  const auto control =
      this->GetRegisters().ReadByIdAs<std::uint64_t>(RegisterID::dr7);

  const auto clear_mask = (0b11 << (index * 2)) | (0b1111 << (index + 16));
  auto       masked     = control & ~clear_mask;

  this->GetRegisters().WriteById(
      RegisterID::dr7, masked);  // write the modified control register
}
