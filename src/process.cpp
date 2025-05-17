#include <libsdb/error.hpp>
#include <libsdb/process.hpp>
#include <csignal>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libsdb/pipe.hpp"

namespace {
  void ExitWithPerror(const sdb::Pipe &channel, std::string const &prefix) {
    const auto message = prefix + ": " + std::strerror(errno);
    channel.Write(reinterpret_cast<const std::byte *>(message.data()),
                  message.size());
    exit(-1);
  }
}  // namespace

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
    const std::filesystem::path &program_path, const bool debug) {
  // we want the pipe to be closed when we call execlp, so we
  // don't leave stale file descriptors
  Pipe channel(/*close_on_exec=*/true);

  pid_t pid = 0;
  if ((pid = fork()) == -1) {
  }

  // if we're in the child process, execute debugee
  if (pid == 0) {
    channel.CloseReadFd();  // we're not using the read end of the pipe

    // attempt to attach to an existing process with the provided PID (only if
    // debugging is enabled)
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
  // process to terminate and send an error message w/ the given message. When
  // there's an error in the child process, the parent process will throw.
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

// Force process to resume and update its tracked running state
void sdb::Process::Resume() {
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
  return stop_reason;
}
