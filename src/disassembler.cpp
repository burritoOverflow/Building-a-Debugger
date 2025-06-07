#include <Zydis/Zydis.h>
#include <libsdb/disassembler.hpp>

std::vector<sdb::Disassembler::Instruction> sdb::Disassembler::Disassemble(
    std::size_t n_instructions, std::optional<VirtualAddress> address) const {
  std::vector<Instruction> ret;
  ret.reserve(n_instructions);

  if (!address) {
    address.emplace(this->process_.GetPc());
  }

  // we're guaranteeing there's enough memory here to disassemble
  // n_instructions, as the largest x86 instruction is 15 bytes.
  const auto code =
      process_.ReadMemoryWithoutTraps(*address, n_instructions * 15);

  ZyanUSize                    offset = 0;
  ZydisDisassembledInstruction instruction;

  while (ZYAN_SUCCESS(ZydisDisassembleATT(
             ZYDIS_MACHINE_MODE_LONG_64, address->GetAddress(),
             code.data() + offset, code.size() - offset, &instruction)) &&
         n_instructions > 0) {
    ret.push_back(Instruction{*address, std::string(instruction.text)});
    offset += instruction.info.length;
    *address += instruction.info.length;
    --n_instructions;
  }

  return ret;
}
