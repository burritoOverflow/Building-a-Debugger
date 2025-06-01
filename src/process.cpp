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
    std::optional<int> stdout_replacement) {
  // we want the pipe to be closed when we call execlp, so we
  // don't leave stale file descriptors
  Pipe channel(/*close_on_exec=*/true);

  pid_t pid = 0;
  if ((pid = fork()) == -1) {
  }

  // if we're in the child process, execute debugee
  if (pid == 0) {
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

  std::unique_ptr<sdb::Process> process(
      new sdb::Process(pid, /*terminate_on_end=*/true, debug));

  if (debug) {
    process->WaitOnSignal();
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

  std::unique_ptr<sdb::Process> process(
      new sdb::Process(pid, /*terminate_on_end=*/false, /*is_attached=*/true));
  process->WaitOnSignal();
  return process;
}

sdb::StopReason sdb::Process::StepInstruction() {
  std::optional<BreakpointSite *> to_reenable;
  auto                            pc = this->GetPc();

  if (this->breakpoint_sites_.EnabledStopPointAtAddress(pc)) {
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

  // and continue the process
  if (ptrace(PTRACE_CONT, this->pid_, nullptr, nullptr) == -1) {
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
  const StopReason stop_reason(wait_status);
  this->state_ = stop_reason.reason;

  if (this->is_attached_ and this->state() == ProcessState::Stopped) {
    // if we're attached to the process, and it's stopped, we
    // read the registers
    this->ReadAllRegisters();

    // if the process stopped due to SIGTRAP and the addr 1 byte below the PC
    // is an enabled breakpoint, we fix up the PC to point to the breakpoint
    const auto instruction_begin = this->GetPc() - 1;
    if (stop_reason.info == SIGTRAP and
        this->breakpoint_sites_.EnabledStopPointAtAddress(instruction_begin)) {
      this->SetPc(instruction_begin);
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
    const VirtualAddress address) {
  if (this->breakpoint_sites_.ContainsAddress(address)) {
    Error::Send("Breakpoint site already created at this address" +
                std::to_string(address.GetAddress()));
  }

  return this->breakpoint_sites_.Push(
      std::unique_ptr<BreakpointSite>(new BreakpointSite(*this, address)));
}

std::vector<std::byte> sdb::Process::ReadMemory(VirtualAddress address,
                                                std::size_t    amount) const {
  std::vector<std::byte> ret(amount);
  iovec                  local_desc{ret.data(), ret.size()};
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

void sdb::Process::WriteMemory(VirtualAddress             address,
                               sdb::Span<const std::byte> data) {
  std::size_t written = 0;

  // until we've written all the data provided by the caller
  while (written < data.Size()) {
    auto remaining = data.Size() - written;

    // data to be written on this iteration
    std::uint64_t word;

    // if at least 8 bytes remain, write the next 8 bytes from the start of
    // the given buffer
    if (remaining >= 8) {
      word = FromBytes<std::uint64_t>(data.begin() + written);
    } else {
      // otherwise, we perform a partial memory write
      // read the 8 bytes we'll be writing to and cast a pointer
      auto read      = ReadMemory(address + written, 8);
      auto word_data = reinterpret_cast<char *>(&word);

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
