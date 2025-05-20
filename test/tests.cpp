#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <fstream>
#include <libsdb/bit.hpp>
#include <libsdb/error.hpp>
#include <libsdb/pipe.hpp>
#include <libsdb/process.hpp>

namespace {
  bool ProcessExists(const pid_t pid) {
    /*
     NOTE:
     From `man 2 kill`
     If sig is 0, then no signal is sent, but existence and permission
     checks are still performed; this can be used to check for the
     existence of a process ID or process group ID that the caller is
     permitted to signal.
    */
    const auto ret = kill(pid, 0);
    // ESRCH - no such process
    return ret != -1 and errno != ESRCH;
  }

  /*
   * Read the process status from the corresponding stat file
   */
  char GetProcessStatus(const pid_t pid) {
    // open the stat file for the given pid
    // we assume the process exists, as we're only to use this function
    // in the context of a running process with the provided PID
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string   data;
    std::getline(stat, data);
    const auto index_of_last_parenthesis = data.rfind(')');
    const auto idx_of_status_indicator   = index_of_last_parenthesis + 2;
    return data[idx_of_status_indicator];
  }
}  // namespace

TEST_CASE("Process::Launch success", "[process]") {
  const auto proc = sdb::Process::Launch("yes");
  REQUIRE(ProcessExists(proc->pid()));
}

TEST_CASE("Process::Launch no such program", "[process]") {
  REQUIRE_THROWS_AS(sdb::Process::Launch("no_such_program"), sdb::Error);
}

TEST_CASE("Process::Attach success", "[process]") {
  // we don't want to attach here...
  // NOTE: when running this test, the executable expects `targets` in cwd
  const auto target = sdb::Process::Launch("targets/run_endlessly", false);
  auto       proc   = sdb::Process::Attach(target->pid());
  REQUIRE(GetProcessStatus(target->pid()) == 't');
}

TEST_CASE("Process::Resume success", "[process]") {
  {
    const auto proc = sdb::Process::Launch("targets/run_endlessly");
    proc->Resume();
    const auto status = GetProcessStatus(proc->pid());
    // either running or sleeping
    const auto success = status == 'R' or status == 'S';
    REQUIRE(success);
  }
  {
    const auto target = sdb::Process::Launch("targets/run_endlessly", false);
    const auto proc   = sdb::Process::Attach(target->pid());
    proc->Resume();
    const auto status  = GetProcessStatus(proc->pid());
    const auto success = status == 'R' or status == 'S';
    REQUIRE(success);
  }
}

TEST_CASE("Process::Resume already terminated", "[process]") {
  // launch the process
  const auto proc = sdb::Process::Launch("targets/end_immediately");
  // resume the process
  proc->Resume();
  // waits for termination
  proc->WaitOnSignal();
  // ensure that calling resume throws an exception
  REQUIRE_THROWS_AS(proc->Resume(), sdb::Error);
}

TEST_CASE("Write register works", "[register]") {
  constexpr bool close_on_exec = false;
  sdb::Pipe      channel(close_on_exec);
  const auto     proc =
      sdb::Process::Launch("targets/reg_write", true, channel.GetWriteFd());
  channel.CloseWriteFd();

  proc->Resume();
  proc->WaitOnSignal();

  // test writing to a general purpose register
  auto &regs = proc->GetRegisters();
  regs.WriteById(sdb::RegisterID::rsi, 0xcafecafe);

  // resume to the next trap
  proc->Resume();
  proc->WaitOnSignal();

  auto output = channel.Read();
  REQUIRE(sdb::ToStringView(output) == "0xcafecafe");

  // same case but for mm0 (MMX register)
  regs.WriteById(sdb::RegisterID::mm0, 0xba5eba11);

  // as above
  proc->Resume();
  proc->WaitOnSignal();

  output = channel.Read();
  REQUIRE(sdb::ToStringView(output) == "0xba5eba11");

  // SSE registers
  regs.WriteById(sdb::RegisterID::xmm0, 42.24);

  // as above
  proc->Resume();
  proc->WaitOnSignal();

  output = channel.Read();
  REQUIRE(sdb::ToStringView(output) == "42.24");
}
