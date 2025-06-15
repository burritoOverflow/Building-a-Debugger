#include <csignal>
#include <editline/readline.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
#include <libsdb/disassembler.hpp>
#include <libsdb/error.hpp>
#include <libsdb/parse.hpp>
#include <libsdb/process.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>


namespace {
  sdb::Process *g_sdb_process = nullptr;  // global process object

  // calls `kill` with the PID of the infernal process
  void HandleSigint(int) { kill(g_sdb_process->GetPid(), SIGSTOP); }

  bool IsPrefix(const std::string_view str, const std::string_view of) {
    if (str.size() > of.size()) {
      return false;
    }
    return std::equal(str.begin(), str.end(), of.begin());
  }

  // show help for commands and the various subcommands
  void PrintHelp(const std::vector<std::string> &args) {
    if (args.size() == 1) {
      std::cerr << R"(Available commands:
        breakpoint - Commands for operating on breakpoints
        continue - Resume the process
        disassemble - Disassemble machine code to assembly
        memory - Commands for operating on memory
        register - Commands for operating on registers
        step - Step over a single instruction
        watchpoint - Commands for operating on watchpoints
)";
    } else if (IsPrefix(args[1], "memory")) {
      std::cerr << R"(Available commands:
        read <address>
        read <address> <number of bytes>
        write <address> <bytes>
)";
    } else if (IsPrefix(args[1], "breakpoint")) {
      std::cerr << R"(Available commands:
        list
        delete <id>
        disable <id>
        enable <id>
        set <address>
        set <address> -h
)";
    } else if (IsPrefix(args[1], "register")) {
      std::cerr << R"(Available commands:
        read
        read <register>
        read all
        write <register> <value>
 )";
    } else if (IsPrefix(args[1], "disassemble")) {
      std::cerr << R"(Available options:
        -c <number of instructions>
        -a <start address>
 )";
    } else if (IsPrefix(args[1], "watchpoint")) {
      std::cerr << R"(Available commands:
        list
        delete <id>
        disable <id>
        enable <id>
        set <address> <write|rw|execute> <size>
)";
    } else {
      std::cerr << "No help available for " << args[1] << '\n';
    }
  }

  std::vector<std::string> Split(const std::string_view str,
                                 const char             delimiter) {
    std::vector<std::string> out{};
    std::stringstream        ss{std::string{str}};
    std::string              item{};

    while (std::getline(ss, item, delimiter)) {
      out.push_back(item);
    }
    return out;
  }

  std::unique_ptr<sdb::Process> Attach(const int argc, char **argv) {
    if (argc == 3 &&
        argv[1] == std::string_view("-p")) {  // passing PID as argument
      const pid_t pid = std::atoi(argv[2]);
      return sdb::Process::Attach(pid);
    }
    // Otherwise, passing program name
    const char *program_path = argv[1];
    auto        proc         = sdb::Process::Launch(program_path);
    fmt::print("Launched process with PID {}\n", proc->GetPid());
    return proc;
  }

  std::string GetSigtrapInfo(const sdb::Process   &process,
                             const sdb::StopReason stop_reason) {
    // for software breakpoints, find the breakpoint site corresponding to the
    // current program counter and generate a string containing the site's ID
    if (stop_reason.trap_reason == sdb::TrapType::SoftwareBreakpoint) {
      const auto &site =
          process.GetBreakpointSites().GetByAddress(process.GetPc());
      return fmt::format(" breakpoint {})", site.GetId());
    }

    if (stop_reason.trap_reason == sdb::TrapType::HardwareBreakpoint) {
      const auto id = process.GetCurrentHardwareStoppoint();

      // hardware breakpoint site
      if (id.index() == 0) {
        const auto &site =
            process.GetBreakpointSites().GetById(std::get<0>(id));
        return fmt::format(" breakpoint {})", site.GetId());
      }

      std::string message;
      const auto &point = process.GetWatchpoints().GetById(std::get<1>(id));
      message += fmt::format(" (watchpoint {})", point.GetId());

      if (point.Data() == point.PreviousData()) {
        message += fmt::format("\nValue: {:#x}", point.Data());
      } else {
        message += fmt::format("\nOld value {:#x}\nNew value {:#x}",
                               point.PreviousData(), point.Data());
      }
      return message;
    }

    if (stop_reason.trap_reason == sdb::TrapType::SingleStep) {
      return " (single step)";
    }

    // TODO: syscall handling
  }

  void PrintStopReason(const sdb::Process    &process,
                       const sdb::StopReason &stop_reason) {
    std::string message;

    switch (stop_reason.reason) {
      case sdb::ProcessState::Exited:
        message = fmt::format("exited with status {}",
                              static_cast<int>(stop_reason.info));
        break;
      case sdb::ProcessState::Terminated:
        message = fmt::format("terminated by signal {}",
                              sigabbrev_np(stop_reason.info));
        break;
      case sdb::ProcessState::Stopped:
        message = fmt::format("stopped by signal {} at {:#x}",
                              sigabbrev_np(stop_reason.info),
                              process.GetPc().GetAddress());
        if (stop_reason.info == SIGTRAP) {
          message += GetSigtrapInfo(process, stop_reason);
        }
        break;
      default:;
    }

    fmt::print("Process {}: {}\n", process.GetPid(), message);
  }


  void PrintDisassembly(sdb::Process &process, sdb::VirtualAddress address,
                        std::size_t n_instructions) {
    const sdb::Disassembler disassembler(process);
    auto instructions = disassembler.Disassemble(n_instructions, address);
    for (const auto &[address, text] : instructions) {
      // add padding for vertical alignment
      fmt::print("{:#18x}: {}\n", address.GetAddress(), text);
    }
  }

  void HandleStop(sdb::Process &process, const sdb::StopReason reason) {
    PrintStopReason(process, reason);
    if (reason.reason == sdb::ProcessState::Stopped) {
      PrintDisassembly(process, process.GetPc(), 10);
    }
  }

  sdb::Registers::value ParseRegisterValue(const sdb::RegisterInfo &info,
                                           const std::string_view   text) {
    try {
      if (info.format == sdb::RegisterFormat::UINT) {
        switch (info.size) {
          case 1:
            {
              return sdb::ToIntegral<std::uint8_t>(text, 16).value();
              case 2:
                return sdb::ToIntegral<std::uint16_t>(text, 16).value();
              case 4:
                return sdb::ToIntegral<std::uint32_t>(text, 16).value();
              case 8:
                return sdb::ToIntegral<std::uint64_t>(text, 16).value();
            }
          default:;
        }
      } else if (info.format == sdb::RegisterFormat::DOUBLE_FLOAT) {
        return sdb::ToFloat<double>(text).value();
      } else if (info.format == sdb::RegisterFormat::LONG_DOUBLE) {
        return sdb::ToFloat<long double>(text).value();
      } else if (info.format == sdb::RegisterFormat::VECTOR) {
        if (info.size == 8) {
          return sdb::ParseVector<8>(text);
        }
        if (info.size == 16) {
          return sdb::ParseVector<16>(text);
        }
      }
    } catch (...) {
      // we're not concerned here (thrown if any of the parsers returns an empty
      // value)
    }
    // if no valid object returned, we throw an error
    sdb::Error::Send("Invalid format");
  }

  void HandleRegisterRead(const sdb::Process             &process,
                          const std::vector<std::string> &args) {
    auto format = [](auto t)
    {
      if constexpr (std::is_floating_point_v<decltype(t)>) {
        return fmt::format("{}", t);
      } else if constexpr (std::is_integral_v<decltype(t)>) {
        // prints a hex with a leading 0x, pads the output with 0s making it 4
        // chars wide 2 chars per byte of the integer and two characters for
        // the leading 0x
        return fmt::format("{:#0{}x}", t, sizeof(t) * 2 + 2);
      } else {
        // join arrays using a comma and format the internal bytes as
        // as hexadecimal with a leading '0x' padded to four characters
        return fmt::format("[{:#04x}]", fmt::join(t, ","));
      }
    };

    if (args.size() == 2 or args.size() == 3 && args[2] == "all") {
      for (auto &info : sdb::gRegisterInfos) {
        // if the user specified all registers or just wants GPRs and the
        // current register is a GPR, we get the value of that register and
        // print it
        const auto should_print =
            (args.size() == 3 || info.type == sdb::RegisterType::GPR) &&
            info.name != "orig_rax";

        if (!should_print) continue;
        auto value = process.GetRegisters().Read(info);
        fmt::print("{}:\t{}\n", info.name, std::visit(format, value));
      }
    } else if (args.size() == 3) {
      try {
        auto info  = sdb::RegisterInfoByName(args[2]);
        auto value = process.GetRegisters().Read(info);
        fmt::print("{}:\t{}\n", info.name, std::visit(format, value));
      } catch (sdb::Error &err) {
        std::cerr << "No such register\n";
      }
    } else {
      PrintHelp({"help", "register"});
    }
  }

  void HandleRegisterWrite(sdb::Process                   &process,
                           const std::vector<std::string> &args) {
    if (args.size() != 4) {
      PrintHelp({"help", "register"});
      return;
    }

    try {
      const auto info  = sdb::RegisterInfoByName(args[2]);
      const auto value = ParseRegisterValue(info, args[3]);
      process.GetRegisters().Write(info, value);
    } catch (...) {
    }
  }

  void HandleRegisterCommand(sdb::Process                   &process,
                             const std::vector<std::string> &args) {
    if (args.size() < 2) {
      PrintHelp({"help", "register"});
    }

    if (IsPrefix(args[1], "read")) {
      HandleRegisterRead(process, args);
    } else if (IsPrefix(args[1], "write")) {
      HandleRegisterWrite(process, args);
    }

    else {
      PrintHelp({"help", "register"});
    }
  }


  void HandleMemoryReadCommand(const sdb::Process             &process,
                               const std::vector<std::string> &args) {
    const auto address = sdb::ToIntegral<std::uint64_t>(args[2], 16);
    if (!address) {
      sdb::Error::Send("Invalid address format");
    }

    auto n_bytes = 32;
    if (args.size() == 4) {
      auto bytes_arg = sdb::ToIntegral<std::size_t>(args[3]);
      if (!bytes_arg) {
        sdb::Error::Send("Invalid number of bytes");
      }
      n_bytes = *bytes_arg;
    }

    auto data = process.ReadMemory(sdb::VirtualAddress{*address}, n_bytes);

    // iterate 16 bytes at a time
    for (std::size_t i = 0; i < data.size(); i += 16) {
      const auto start = data.begin() + i;

      // offset the start of the data by smaller of (1 + 16) and the total data
      // size (ensuring that `end` doesn't point past the end of the data of the
      // number of bytes is not divisible by 16)
      const auto end = data.begin() + std::min(i + 16, data.size());

      // show in hex with a padding of 16 chars and show each value in hex
      // without leading '0x'
      fmt::print("{:#016x}: {:02x}\n", *address + i,
                 fmt::join(start, end, " "));
    }
  }

  void HandleMemoryWriteCommand(sdb::Process                   &process,
                                const std::vector<std::string> &args) {
    // memory write command expects 4 arguments:
    // i.e 'mem write 0x555555555156 [0xff,0xce]'
    // where this last argument is a vector of bytes in hexadecimal, with no
    // space between commas. if a user provides spaces, we'll land on this
    // failure branch
    if (args.size() != 4) {
      PrintHelp({"help", "memory"});
      return;
    }

    // parse the address, i,e '0x555555555156'
    const auto address = sdb::ToIntegral<std::uint64_t>(args[2], 16);
    if (!address) {
      sdb::Error::Send("Invalid address format");
    }

    const auto data = sdb::ParseVector(args[3]);
    process.WriteMemory(sdb::VirtualAddress{*address},
                        {data.data(), data.size()});
  }

  void HandleMemoryCommand(sdb::Process                   &process,
                           const std::vector<std::string> &args) {
    if (args.size() < 3) {
      PrintHelp({"help", "memory"});
      return;
    }

    if (IsPrefix(args[1], "read")) {
      HandleMemoryReadCommand(process, args);
    } else if (IsPrefix(args[1], "write")) {
      HandleMemoryWriteCommand(process, args);
    } else {
      PrintHelp({"help", "memory"});
    }
  }

  void HandleBreakpointCommand(
      std::unique_ptr<sdb::Process>::element_type &process,
      const std::vector<std::string>              &args) {
    if (args.size() < 2) {
      PrintHelp({"help", "breakpoint"});
      return;
    }

    const auto &command = args[1];

    if (IsPrefix(command, "list")) {
      if (process.GetBreakpointSites().IsEmpty()) {
        fmt::print("No breakpoints set\n");
      } else {
        fmt::print("Current breakpoints:\n");
        process.GetBreakpointSites().ForEach(
            [](const auto &site)
            {
              // skip internal breakpoints
              if (site.IsInternal()) {
                return;
              }
              fmt::print("{}: address - {:#x}, {}\n", site.GetId(),
                         site.Address().GetAddress(),
                         site.IsEnabled() ? "enabled" : "disabled");
            });
      }
    }

    if (args.size() < 3) {
      PrintHelp({"help", "breakpoint"});
      return;
    }

    if (IsPrefix(command, "set")) {
      const auto address = sdb::ToIntegral<std::uint64_t>(args[2], 16);

      if (!address) {
        fmt::print(stderr,
                   "Breakpoint command expects address "
                   "in hexadecimal, prefixed with '0x'\n");
        return;
      }

      bool hardware = false;
      if (args.size() == 4) {
        // hardware flag -- set hardware breakpoint if the user specified it.
        if (args[3] == "-h") {
          hardware = true;
        } else {
          sdb::Error::Send("Invalid breakpoint command argument");
        }
      }

      process.CreateBreakpointSite(sdb::VirtualAddress{*address}, hardware)
          .Enable();
      return;
    }

    const auto id = sdb::ToIntegral<sdb::BreakpointSite::id_type>(args[2]);
    if (!id) {
      std::cerr << "Breakpoint command expects breakpoint ID in decimal\n";
      return;
    }

    // Handle remaining cases for enabling, disabling and deleting
    if (IsPrefix(command, "enable")) {
      process.GetBreakpointSites().GetById(*id).Enable();
    } else if (IsPrefix(command, "disable")) {
      process.GetBreakpointSites().GetById(*id).Disable();
    } else if (IsPrefix(command, "delete")) {
      process.GetBreakpointSites().RemoveById(*id);
    }
  }

  void HandleWatchpointList(sdb::Process                   &process,
                            const std::vector<std::string> &args) {
    const auto StoppointModeToStr = [](const sdb::StoppointMode mode)
    {
      switch (mode) {
        case sdb::StoppointMode::execute:
          return "execute";
        case sdb::StoppointMode::write:
          return "write";
        case sdb::StoppointMode::read_write:
          return "read_write";
        default:
          sdb::Error::Send("Invalid stoppoint mode");
      }
    };

    if (process.GetWatchpoints().IsEmpty()) {
      fmt::print("No watchpoints set\n");
    } else {
      fmt::print("Current watchpoints:\n");
      process.GetWatchpoints().ForEach(
          [&](const auto &watchpoint)
          {
            fmt::print("{}: address = {:#x}, mode = {}, size = {}, {}\n",
                       watchpoint.GetId(), watchpoint.GetAddress().GetAddress(),
                       StoppointModeToStr(watchpoint.GetMode()),
                       watchpoint.GetSize(),
                       watchpoint.IsEnabled() ? "enabled" : "disabled");
          });
    }
  }

  void HandleWatchpointSet(sdb::Process                   &process,
                           const std::vector<std::string> &args) {
    if (args.size() != 5) {
      PrintHelp({"help", "watchpoint"});
      return;
    }

    const auto  address   = sdb::ToIntegral<std::uint64_t>(args[2], 16);
    const auto &mode_text = args[3];
    const auto  size      = sdb::ToIntegral<std::size_t>(args[4]);

    if (!address || !size ||
        !(mode_text == "execute" || mode_text == "write" ||
          mode_text == "rw")) {
      PrintHelp({"help", "watchpoint"});
      return;
    }

    sdb::StoppointMode mode;
    // we've validated that it's one of these three above, so safe to
    // use after this point
    if (mode_text == "execute") {
      mode = sdb::StoppointMode::execute;
    } else if (mode_text == "write") {
      mode = sdb::StoppointMode::write;
    } else if (mode_text == "rw") {
      mode = sdb::StoppointMode::read_write;
    }

    process.CreateWatchpoint(sdb::VirtualAddress{*address}, mode, *size)
        .Enable();
  }

  void HandleWatchpointCommand(
      std::unique_ptr<sdb::Process>::element_type &process,
      const std::vector<std::string>              &args) {
    if (args.size() < 2) {
      PrintHelp({"help", "watchpoint"});
      return;
    }

    const auto &command = args[1];

    if (IsPrefix(command, "list")) {
      HandleWatchpointList(process, args);
      return;
    }

    if (IsPrefix(command, "set")) {
      HandleWatchpointSet(process, args);
      return;
    }

    if (args.size() < 3) {
      PrintHelp({"help", "watchpoint"});
      return;
    }

    const auto id = sdb::ToIntegral<sdb::Watchpoint::id_type>(args[2]);
    if (!id) {
      std::cerr << "Watchpoint command expects watchpoint ID\n";
    }

    if (IsPrefix(command, "enable")) {
      process.GetWatchpoints().GetById(*id).Enable();
    } else if (IsPrefix(command, "disable")) {
      process.GetWatchpoints().GetById(*id).Disable();
    } else if (IsPrefix(command, "delete")) {
      process.GetWatchpoints().RemoveById(*id);
    }
  }

  void HandleDisassembleCommand(sdb::Process                   &process,
                                const std::vector<std::string> &args) {
    auto        address        = process.GetPc();
    std::size_t n_instructions = 5;

    auto it = args.begin() + 1;

    while (it != args.end()) {
      if (*it == "-a" && it + 1 != args.end()) {
        ++it;

        const auto opt_address = sdb::ToIntegral<std::uint64_t>(*it++, 16);
        if (!opt_address) {
          sdb::Error::Send("Invalid address format");
        }

        address = sdb::VirtualAddress{*opt_address};
      } else if (*it == "-c" && it + 1 != args.end()) {
        ++it;
        auto opt_n = sdb::ToIntegral<std::size_t>(*it++);
        if (!opt_n) {
          sdb::Error::Send("Invalid instruction count");
        }
        n_instructions = *opt_n;
      } else {
        PrintHelp({"help", "disassemble"});
        return;
      }
    }
    PrintDisassembly(process, address, n_instructions);
  }

  void HandleCommand(const std::unique_ptr<sdb::Process> &process,
                     const std::string_view               line) {
    const auto  args    = Split(line, ' ');
    const auto &command = args[0];

    if (IsPrefix(command, "continue")) {
      process->Resume();
      const auto reason = process->WaitOnSignal();
      HandleStop(*process, reason);
    } else if (IsPrefix(command, "memory")) {
      HandleMemoryCommand(*process, args);
    } else if (IsPrefix(command, "register")) {
      HandleRegisterCommand(*process, args);
    } else if (IsPrefix(command, "breakpoint")) {
      HandleBreakpointCommand(*process, args);
    } else if (IsPrefix(command, "watchpoint")) {
      HandleWatchpointCommand(*process, args);
    } else if (IsPrefix(command, "step")) {
      const auto reason = process->StepInstruction();
      HandleStop(*process, reason);
    } else if (IsPrefix(command, "help")) {
      PrintHelp(args);
    } else if (IsPrefix(command, "disassemble")) {
      HandleDisassembleCommand(*process, args);
    } else {
      std::cerr << "Unknown command\n";
    }
  }

  void MainLoop(const std::unique_ptr<sdb::Process> &process) {
    char *line = nullptr;
    while ((line = readline("sdb> ")) != nullptr) {
      // command to be executed, regardless of where it came from--either the
      // user input or readline history
      std::string line_string{};

      if (line == std::string_view("")) {
        free(line);
        if (history_length > 0) {
          // if the history is not empty, get the last command
          line_string = history_list()[history_length - 1]->line;
        }
      } else {
        line_string = line;
        add_history(line);
        free(line);
      }

      if (!line_string.empty()) {
        try {
          HandleCommand(process, line_string);
        } catch (const sdb::Error &err) {
          std::cerr << err.what() << '\n';
        }
      }
    }
  }

}  // namespace

int main(const int argc, char **argv) {
  if (argc == 1) {
    std::cerr << "No arguments given\n";
    return -1;
  }

  try {
    const auto process = Attach(argc, argv);
    // install the signal handler
    g_sdb_process = process.get();
    signal(SIGINT, HandleSigint);
    MainLoop(process);
  } catch (const sdb::Error &err) {
    std::cerr << err.what() << '\n';
  }

  return 0;
}
