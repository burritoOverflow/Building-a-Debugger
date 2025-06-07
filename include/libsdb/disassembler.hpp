#ifndef SDB_DISASSEMBLER_HPP
#define SDB_DISASSEMBLER_HPP

#include <libsdb/process.hpp>
#include <libsdb/types.hpp>
#include <string>


namespace sdb {
  class Disassembler {
    struct Instruction {
      VirtualAddress
          address;  // memory address where the binary instruction is stored
      std::string text;  // string representation of the instruction
    };

public:
    explicit Disassembler(Process &process) : process_(process) {}

    std::vector<Instruction> Disassemble(
        std::size_t                   n_instructions,
        std::optional<VirtualAddress> address =
            std::nullopt) const;  // by default, we'll use the
                                  // current program counter's value

private:
    Process &process_;
  };
}  // namespace sdb

#endif  // SDB_DISASSEMBLER_HPP
