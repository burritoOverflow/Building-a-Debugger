#include <editline/readline.h>
#include <iostream>
#include <libsdb/error.hpp>
#include <libsdb/process.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
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

  bool IsPrefix(const std::string_view str, std::string_view of) {
    if (str.size() < of.size()) {
      return false;
    }
    return std::equal(str.begin(), str.end(), of.begin());
  }

  std::unique_ptr<sdb::Process> Attach(const int argc, char **argv) {
    if (argc == 3 &&
        argv[1] == std::string_view("-p")) {  // passing PID as argument
      const pid_t pid = std::atoi(argv[2]);
      return sdb::Process::Attach(pid);
    }
    // Otherwise, passing program name
    const char *program_path = argv[1];
    return sdb::Process::Launch(program_path);
  }

  void PrintStopReason(const sdb::Process   &process,
                       const sdb::StopReason stop_reason) {
    std::cout << "Process " << process.pid() << ' ';

    switch (stop_reason.reason) {
      case sdb::ProcessState::Exited:
        std::cout << "exited with status "
                  << static_cast<int>(stop_reason.info);
        break;
      case sdb::ProcessState::Terminated:
        std::cout << "terminated with signal "
                  << sigabbrev_np(stop_reason.info);
        break;
      case sdb::ProcessState::Stopped:
        std::cout << "stopped with signal " << sigabbrev_np(stop_reason.info);
        break;
      default:;
    }

    std::cout << std::endl;
  }

  void HandleCommand(const std::unique_ptr<sdb::Process> &process,
                     const std::string_view               line) {
    const auto  args    = Split(line, ' ');
    const auto &command = args[0];

    if (IsPrefix(command, "continue")) {
      process->Resume();
      const auto reason = process->WaitOnSignal();
      PrintStopReason(*process, reason);
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
