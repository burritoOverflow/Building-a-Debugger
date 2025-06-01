#include <editline/readline.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
#include <libsdb/error.hpp>
#include <libsdb/parse.hpp>
#include <libsdb/process.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>


namespace {
  bool IsPrefix(const std::string_view str, std::string_view of) {
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
        memory - Commands for operating on memory
        register - Commands for operating on registers
        step - Step over a single instruction
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
)";
    } else if (IsPrefix(args[1], "register")) {
      std::cerr << R"(Available commands:
        read
        read <register>
        read all
        write <register> <value>
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

  void PrintStopReason(const sdb::Process   &process,
                       const sdb::StopReason stop_reason) {
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
        break;
      default:;
    }

    fmt::print("Process {}: {}\n", process.GetPid(), message);
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
        // as hexadecimal with a leading '0x padded to four characters
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
      auto info  = sdb::RegisterInfoByName(args[2]);
      auto value = ParseRegisterValue(info, args[3]);
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

    auto command = args[1];

    if (IsPrefix(command, "list")) {
      if (process.GetBreakpointSites().IsEmpty()) {
        fmt::print("No breakpoints set\n");
      } else {
        fmt::print("Current breakpoints:\n");
        process.GetBreakpointSites().ForEach(
            [](const auto &site)
            {
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
      auto address = sdb::ToIntegral<std::uint64_t>(args[2], 16);

      if (!address) {
        fmt::print(stderr,
                   "Breakpoint command expects address "
                   "in hexadecimal, prefixed with '0x'\n");
        return;
      }

      process.CreateBreakpointSite(sdb::VirtualAddress{*address}).Enable();
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
    } else if (IsPrefix(command, "diable")) {
      process.GetBreakpointSites().GetById(*id).Disable();
    } else if (IsPrefix(command, "delete")) {
      process.GetBreakpointSites().RemoveById(*id);
    }
  }

  void HandleCommand(const std::unique_ptr<sdb::Process> &process,
                     const std::string_view               line) {
    const auto  args    = Split(line, ' ');
    const auto &command = args[0];

    if (IsPrefix(command, "continue")) {
      process->Resume();
      const auto reason = process->WaitOnSignal();
      PrintStopReason(*process, reason);
    } else if (IsPrefix(command, "memory")) {
      HandleMemoryCommand(*process, args);
    } else if (IsPrefix(command, "register")) {
      HandleRegisterCommand(*process, args);
    } else if (IsPrefix(command, "breakpoint")) {
      HandleBreakpointCommand(*process, args);
    } else if (IsPrefix(command, "step")) {
      const auto reason = process->StepInstruction();
      PrintStopReason(*process, reason);
    } else if (IsPrefix(command, "help")) {
      PrintHelp(args);
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
    MainLoop(process);
  } catch (const sdb::Error &err) {
    std::cerr << err.what() << '\n';
  }

  return 0;
}
