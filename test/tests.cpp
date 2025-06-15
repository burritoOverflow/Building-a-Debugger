#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <libsdb/bit.hpp>
#include <libsdb/breakpoint_site.hpp>
#include <libsdb/error.hpp>
#include <libsdb/pipe.hpp>
#include <libsdb/process.hpp>
#include <libsdb/syscalls.hpp>
#include <libsdb/types.hpp>
#include <regex>

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

  std::int64_t GetSectionLoadBias(const std::filesystem::path &path,
                                  const Elf64_Addr             file_address) {
    const auto       command = std::string("readelf -WS ") + path.string();
    const auto       pipe    = popen(command.c_str(), "r");
    const std::regex text_regex(R"(PROGBITS\s+(\w+)\s+(\w+)\s+(\w+))");
    char            *line = nullptr;
    std::size_t      len  = 0;

    /*
      Something like:
      [16] .text             PROGBITS        0000000000001060 001060 000107 00
      AX  0   0 16 The capture group in the block that follows will capture the
      three whitespace-separated numbers that succeed 'PROGBITS'.
      */
    while (getline(&line, &len, pipe) != -1) {
      if (std::cmatch groups; std::regex_search(line, groups, text_regex)) {
        const auto address = std::stol(groups[1], nullptr, 16);
        const auto offset  = std::stol(groups[2], nullptr, 16);
        const auto size    = std::stol(groups[3], nullptr, 16);

        // If the given file address lies in the range of the section address
        // plus the section size, we clean up and return the section load bias.
        if (address <= file_address and file_address < (address + size)) {
          free(line);
          pclose(pipe);
          // return the load bias
          return address - offset;
        }
      }
      free(line);
      line = nullptr;
    }
    pclose(pipe);
    sdb::Error::Send("Could not find the section load bias");
  }

  std::int64_t GetEntryPointOffset(const std::filesystem::path &path) {
    std::ifstream elf_file(path);

    Elf64_Ehdr header;
    elf_file.read(reinterpret_cast<char *>(&header), sizeof(header));

    auto entry_file_address = header.e_entry;
    auto load_bias          = GetSectionLoadBias(path, entry_file_address);

    return entry_file_address - load_bias;
  }

  sdb::VirtualAddress GetLoadAddress(pid_t pid, std::int64_t offset) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    // we're looking to match lines that look like this:
    // 555555555000-555555556000 r-xp 00001000
    std::regex map_regex(R"((\w+)-\w+ ..(.). (\w+))");

    std::string data;

    while (std::getline(maps, data)) {
      std::smatch groups;
      std::regex_search(data, groups, map_regex);

      if (groups[2] == 'x') {  // line that is marked executable
        auto low_range   = std::stol(groups[1], nullptr, 16);
        auto file_offset = std::stol(groups[3], nullptr, 16);
        // calculate the load address of the file offset by subtracting the file
        // offset of the loaded segment w/in the original object file and adding
        // the low virtual address of the mapped segment
        return sdb::VirtualAddress(offset - file_offset + low_range);
      }
    }
    sdb::Error::Send("Could not find load address for the given PID");
  }
}  // namespace

TEST_CASE("Process::Launch success", "[process]") {
  const auto proc = sdb::Process::Launch("yes");
  REQUIRE(ProcessExists(proc->GetPid()));
}

TEST_CASE("Process::Launch no such program", "[process]") {
  REQUIRE_THROWS_AS(sdb::Process::Launch("no_such_program"), sdb::Error);
}

TEST_CASE("Process::Attach success", "[process]") {
  // we don't want to attach here...
  // NOTE: when running this test, the executable expects `targets` in cwd
  const auto target = sdb::Process::Launch("targets/run_endlessly", false);
  auto       proc   = sdb::Process::Attach(target->GetPid());
  REQUIRE(GetProcessStatus(target->GetPid()) == 't');
}

TEST_CASE("Process::Resume success", "[process]") {
  {
    const auto proc = sdb::Process::Launch("targets/run_endlessly");
    proc->Resume();
    const auto status = GetProcessStatus(proc->GetPid());
    // either running or sleeping
    const auto success = status == 'R' or status == 'S';
    REQUIRE(success);
  }
  {
    const auto target = sdb::Process::Launch("targets/run_endlessly", false);
    const auto proc   = sdb::Process::Attach(target->GetPid());
    proc->Resume();
    const auto status  = GetProcessStatus(proc->GetPid());
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

  // st0 - location where the value is stored.
  regs.WriteById(sdb::RegisterID::st0, 42.24l);
  /*
   * fsw - (FPU Status Word) - 16 bits wide - 11-13 track the top of the stack
   *
   * NOTE: (from book, verbatim) The top of the stack starts at index 0 and
   * goes down instead of up, wrapping around up to 7.
   * So, to push our value to the stack, we set bits 11 through 13 to 7 (0b111):
   */
  regs.WriteById(sdb::RegisterID::ftw, std::uint16_t{0b0011100000000000});

  /*
   * ftw - (FPU Tag Word)
   * Also verbatim from the book:
   *
   * The 16-bit tag register tracks which of the st registers are
   * valid, empty, or special (meaning they contain NaNs or infinity). A tag of
   * 0b11 means empty, 0b00 means valid. So, we want to set the first tag to
   * 0b00 and the rest to 0b11:
   */
  regs.WriteById(sdb::RegisterID::ftw, std::uint16_t{0b0011111111111111});

  proc->Resume();
  proc->WaitOnSignal();

  output = channel.Read();
  REQUIRE(sdb::ToStringView(output) == "42.24");
}

TEST_CASE("Read register works", "[register]") {
  const auto  proc = sdb::Process::Launch("targets/reg_read");
  const auto &regs = proc->GetRegisters();

  proc->Resume();
  proc->WaitOnSignal();

  REQUIRE(regs.ReadByIdAs<std::uint64_t>(sdb::RegisterID::r13) == 0xcafecafe);

  proc->Resume();
  proc->WaitOnSignal();

  // 8-bit subregister
  REQUIRE(regs.ReadByIdAs<std::uint8_t>(sdb::RegisterID::r13b) == 42);

  proc->Resume();
  proc->WaitOnSignal();

  REQUIRE(regs.ReadByIdAs<sdb::byte64>(sdb::RegisterID::mm0) ==
          sdb::ToByte64(0xba5eba11ull));

  proc->Resume();
  proc->WaitOnSignal();

  REQUIRE(regs.ReadByIdAs<sdb::byte128>(sdb::RegisterID::xmm0) ==
          sdb::ToByte128(64.125));

  proc->Resume();
  proc->WaitOnSignal();

  REQUIRE(regs.ReadByIdAs<long double>(sdb::RegisterID::st0) == 64.125L);
}

TEST_CASE("Can create breakpoint site", "[breakpoint]") {
  // verify that we can create a breakpoint site at a given address
  // and that the registered address is the same as the one we provided
  const auto  proc = sdb::Process::Launch("targets/run_endlessly");
  const auto &site = proc->CreateBreakpointSite(sdb::VirtualAddress{42});
  REQUIRE(site.Address().GetAddress() == 42);
}

// verify that each additional breakpoint site increments the id
TEST_CASE("Breakpoint site ids increase", "[breakpoint]") {
  const auto proc = sdb::Process::Launch("targets/run_endlessly");

  const auto &s1 = proc->CreateBreakpointSite(sdb::VirtualAddress{42});
  REQUIRE(s1.Address().GetAddress() == 42);

  // subsequent breakpoint sites should have ids one greater than the previous
  const auto &s2 = proc->CreateBreakpointSite(sdb::VirtualAddress{43});
  REQUIRE(s2.Address().GetAddress() == 43);
  REQUIRE(s2.GetId() == s1.GetId() + 1);

  const auto &s3 = proc->CreateBreakpointSite(sdb::VirtualAddress{44});
  REQUIRE(s3.Address().GetAddress() == 44);
  REQUIRE(s3.GetId() == s1.GetId() + 2);

  const auto &s4 = proc->CreateBreakpointSite(sdb::VirtualAddress{45});
  REQUIRE(s4.Address().GetAddress() == 45);
  REQUIRE(s4.GetId() == s1.GetId() + 3);
}

TEST_CASE("Can find breakpoint sites", "[breakpoint]") {
  auto        proc  = sdb::Process::Launch("targets/run_endlessly");
  const auto &cproc = proc;

  proc->CreateBreakpointSite(sdb::VirtualAddress{42});
  proc->CreateBreakpointSite(sdb::VirtualAddress{43});
  proc->CreateBreakpointSite(sdb::VirtualAddress{44});
  proc->CreateBreakpointSite(sdb::VirtualAddress{45});

  auto &s1 = proc->GetBreakpointSites().GetByAddress(sdb::VirtualAddress{44});
  REQUIRE(proc->GetBreakpointSites().ContainsAddress(sdb::VirtualAddress{44}));
  REQUIRE(s1.Address().GetAddress() == 44);

  auto &cs1 = cproc->GetBreakpointSites().GetByAddress(sdb::VirtualAddress{44});
  REQUIRE(cproc->GetBreakpointSites().ContainsAddress(sdb::VirtualAddress{44}));
  REQUIRE(cs1.Address().GetAddress() == 44);

  auto &s2 = proc->GetBreakpointSites().GetById(s1.GetId() + 1);
  REQUIRE(proc->GetBreakpointSites().ContainsId(s1.GetId() + 1));
  REQUIRE(s2.GetId() == s1.GetId() + 1);
  REQUIRE(s2.Address().GetAddress() == 45);

  auto &cs2 = cproc->GetBreakpointSites().GetById(cs1.GetId() + 1);
  REQUIRE(cproc->GetBreakpointSites().ContainsId(cs1.GetId() + 1));
  REQUIRE(cs2.GetId() == cs1.GetId() + 1);
  REQUIRE(cs2.Address().GetAddress() == 45);
}

TEST_CASE("Cannot find non-existing breakpoint sites", "[breakpoint]") {
  auto        proc  = sdb::Process::Launch("targets/run_endlessly");
  const auto &cproc = proc;

  // verify that these methods throw if provided a breakpoint site that does not
  // exist
  REQUIRE_THROWS_AS(
      proc->GetBreakpointSites().GetByAddress(sdb::VirtualAddress{44}),
      sdb::Error);
  REQUIRE_THROWS_AS(cproc->GetBreakpointSites().GetById(44), sdb::Error);
  REQUIRE_THROWS_AS(
      cproc->GetBreakpointSites().GetByAddress(sdb::VirtualAddress{44}),
      sdb::Error);
  REQUIRE_THROWS_AS(cproc->GetBreakpointSites().GetById(44), sdb::Error);
}

TEST_CASE("Breakpoint list size and emptiness", "[breakpoint]") {
  auto        proc  = sdb::Process::Launch("targets/run_endlessly");
  const auto &cproc = proc;

  REQUIRE(proc->GetBreakpointSites().IsEmpty());
  REQUIRE(proc->GetBreakpointSites().Size() == 0);
  REQUIRE(cproc->GetBreakpointSites().IsEmpty());
  REQUIRE(cproc->GetBreakpointSites().Size() == 0);

  proc->CreateBreakpointSite(sdb::VirtualAddress{42});
  REQUIRE(!proc->GetBreakpointSites().IsEmpty());
  REQUIRE(proc->GetBreakpointSites().Size() == 1);
  REQUIRE(!cproc->GetBreakpointSites().IsEmpty());
  REQUIRE(cproc->GetBreakpointSites().Size() == 1);

  proc->CreateBreakpointSite(sdb::VirtualAddress{43});
  REQUIRE(!proc->GetBreakpointSites().IsEmpty());
  REQUIRE(proc->GetBreakpointSites().Size() == 2);
  REQUIRE(!cproc->GetBreakpointSites().IsEmpty());
  REQUIRE(cproc->GetBreakpointSites().Size() == 2);
}

TEST_CASE("Can iterate breakpoint sizes", "[breakpoint]") {
  auto        proc  = sdb::Process::Launch("targets/run_endlessly");
  const auto &cproc = proc;

  proc->CreateBreakpointSite(sdb::VirtualAddress{42});
  proc->CreateBreakpointSite(sdb::VirtualAddress{43});
  proc->CreateBreakpointSite(sdb::VirtualAddress{44});
  proc->CreateBreakpointSite(sdb::VirtualAddress{45});

  proc->GetBreakpointSites().ForEach(
      [addr = 42](auto &site) mutable
      { REQUIRE(site.Address().GetAddress() == addr++); });

  cproc->GetBreakpointSites().ForEach(
      [addr = 42](const auto &site) mutable
      { REQUIRE(site.Address().GetAddress() == addr++); });
}

TEST_CASE("Breakpoint on address works", "[breakpoint]") {
  bool      close_on_exec = false;
  sdb::Pipe channel(close_on_exec);

  const std::filesystem::path target_path = "targets/hello_sdb";

  auto proc = sdb::Process::Launch(target_path, true, channel.GetWriteFd());
  channel.CloseWriteFd();

  const auto offset       = GetEntryPointOffset(target_path);
  const auto load_address = GetLoadAddress(proc->GetPid(), offset);

  proc->CreateBreakpointSite(load_address).Enable();
  proc->Resume();
  auto reason = proc->WaitOnSignal();

  REQUIRE(reason.reason == sdb::ProcessState::Stopped);
  REQUIRE(reason.info == SIGTRAP);
  REQUIRE(proc->GetPc() == load_address);

  proc->Resume();
  reason = proc->WaitOnSignal();

  REQUIRE(reason.reason == sdb::ProcessState::Exited);
  // check that the process exited successfully
  REQUIRE(reason.info == 0);

  const auto data = channel.Read();
  // expected output sent to stdout for this test target
  REQUIRE(sdb::ToStringView(data) == "Hello, sdb!\n");
}

TEST_CASE("Can remove breakpoint sites", "[breakpoint]") {
  const auto  proc = sdb::Process::Launch("targets/run_endlessly");
  const auto &site = proc->CreateBreakpointSite(sdb::VirtualAddress{42});
  proc->CreateBreakpointSite(sdb::VirtualAddress{43});

  REQUIRE(proc->GetBreakpointSites().Size() == 2);

  // verify both remove APIs function as intended
  proc->GetBreakpointSites().RemoveById(site.GetId());
  REQUIRE(proc->GetBreakpointSites().Size() == 1);
  proc->GetBreakpointSites().RemoveByAddress(sdb::VirtualAddress{43});
  REQUIRE(proc->GetBreakpointSites().IsEmpty());
}

TEST_CASE("Reading and writing memory works", "[memory]") {
  constexpr bool close_on_exec = false;
  sdb::Pipe      channel(close_on_exec);
  const auto     proc =
      sdb::Process::Launch("targets/memory", true, channel.GetWriteFd());
  channel.CloseWriteFd();

  proc->Resume();
  proc->WaitOnSignal();

  // our test program writes the pointer's address to the channel
  // we read this value, get the memory at that address, and verify that it is
  // as expected '0xcafecafe'
  const auto a_pointer = sdb::FromBytes<std::uint64_t>(channel.Read().data());
  const auto data_vec  = proc->ReadMemory(sdb::VirtualAddress{a_pointer}, 8);
  const auto data      = sdb::FromBytes<std::uint64_t>(data_vec.data());

  REQUIRE(data == 0xcafecafe);

  proc->Resume();
  proc->WaitOnSignal();

  // as above, but we write to the memory at the pointer's address
  const auto b_pointer = sdb::FromBytes<std::uint64_t>(channel.Read().data());
  proc->WriteMemory(sdb::VirtualAddress{b_pointer},
                    {sdb::AsBytes("Hello, sdb!"), 12});

  proc->Resume();
  proc->WaitOnSignal();

  const auto read = channel.Read();
  REQUIRE(sdb::ToStringView(read) == "Hello, sdb!");
}

TEST_CASE("Hardware breakpoint evades memory checksums", "[breakpoint]") {
  constexpr bool close_on_exec = false;
  sdb::Pipe      channel(close_on_exec);
  const auto     proc =
      sdb::Process::Launch("targets/anti_debugger", true, channel.GetWriteFd());
  channel.CloseWriteFd();

  proc->Resume();
  proc->WaitOnSignal();

  // get the address (sent via stdout) of the 'innocent' function
  const auto func =
      sdb::VirtualAddress(sdb::FromBytes<std::uint64_t>(channel.Read().data()));

  // and set a software breakpoint at the function's address
  auto &soft = proc->CreateBreakpointSite(func, false);
  soft.Enable();

  proc->Resume();
  proc->WaitOnSignal();

  REQUIRE(sdb::ToStringView(channel.Read()) ==
          "Putting pepperoni on pizza...\n");

  // remove the software breakpoint
  proc->GetBreakpointSites().RemoveById(soft.GetId());

  // and now set a hardware breakpoint at the same address
  auto &hard = proc->CreateBreakpointSite(func, true);
  hard.Enable();

  proc->Resume();
  proc->WaitOnSignal();

  REQUIRE(proc->GetPc() == func);

  proc->Resume();
  proc->WaitOnSignal();

  // and last, verify that the 'innocent' function is being called
  REQUIRE(sdb::ToStringView(channel.Read()) ==
          "Putting pineapple on pizza...\n");
}

TEST_CASE("Watchpoint detects read", "[watchpoint]") {
  constexpr bool close_on_exec = false;
  sdb::Pipe      channel(close_on_exec);
  const auto     proc =
      sdb::Process::Launch("targets/anti_debugger", true, channel.GetWriteFd());
  channel.CloseWriteFd();

  proc->Resume();
  proc->WaitOnSignal();

  const auto func =
      sdb::VirtualAddress(sdb::FromBytes<std::uint64_t>(channel.Read().data()));

  auto &watch = proc->CreateWatchpoint(func, sdb::StoppointMode::read_write, 1);
  watch.Enable();

  proc->Resume();
  proc->WaitOnSignal();

  proc->StepInstruction();
  auto &soft = proc->CreateBreakpointSite(func, false);
  soft.Enable();

  proc->Resume();
  const auto reason = proc->WaitOnSignal();

  REQUIRE(reason.info == SIGTRAP);

  proc->Resume();
  proc->WaitOnSignal();

  // as above.
  REQUIRE(sdb::ToStringView(channel.Read()) ==
          "Putting pineapple on pizza...\n");
}

// Verify that the syscall mapping functions for both conversions
TEST_CASE("Syscall mapping works", "[syscall]") {
  REQUIRE(sdb::SyscallIdToName(0) == "read");
  REQUIRE(sdb::SyscallNameToId("read") == 0);
  REQUIRE(sdb::SyscallIdToName(326) == "copy_file_range");
  REQUIRE(sdb::SyscallNameToId("copy_file_range") == 326);
  REQUIRE(sdb::SyscallIdToName(62) == "kill");
  REQUIRE(sdb::SyscallNameToId("kill") == 62);
}

TEST_CASE("Syscall catchpoint work", "[syscall]") {
  auto       dev_null = open("/dev/null", O_WRONLY);
  const auto proc =
      sdb::Process::Launch("targets/anti_debugger", true, dev_null);

  auto write_syscall = sdb::SyscallNameToId("write");
  auto policy        = sdb::SyscallCatchPolicy::CatchSome({write_syscall});
  proc->SetSyscallCatchPolicy(std::move(policy));

  proc->Resume();
  auto reason = proc->WaitOnSignal();

  REQUIRE(reason.reason == sdb::ProcessState::Stopped);
  REQUIRE(reason.info == SIGTRAP);
  REQUIRE(reason.trap_reason == sdb::TrapType::Syscall);
  REQUIRE(reason.syscall_info->id == write_syscall);
  REQUIRE(reason.syscall_info->entry == true);

  proc->Resume();
  reason = proc->WaitOnSignal();

  REQUIRE(reason.reason == sdb::ProcessState::Stopped);
  REQUIRE(reason.info == SIGTRAP);
  REQUIRE(reason.trap_reason == sdb::TrapType::Syscall);
  REQUIRE(reason.syscall_info->id == write_syscall);
  REQUIRE(reason.syscall_info->entry == false);

  close(dev_null);
}
