#include <libsdb/elf.hpp>
#include <libsdb/types.hpp>

sdb::VirtualAddress sdb::FileAddress::ToVirtualAddress(const Elf& obj) const {
  assert(elf_ && "ToVirtualAddress called on null address");
  // locate the section that contains the file address to ensure that this file
  // address is valid for the ELF file
  if (const auto section = elf_->GetSectionContainingAddress(*this); !section) {
    // if there isn't one, we'll return an empty virtual address
    return VirtualAddress{};
  }
  // otherwise, add the stored address to the load bias to compute the real
  // virtual address
  return VirtualAddress{addr_ + elf_->GetLoadBias().GetAddress()};
}

sdb::FileAddress sdb::VirtualAddress::ToFileAddress(const Elf& obj) const {
  if (const auto section = obj.GetSectionContainingAddress(*this); !section) {
    return FileAddress();
  }
  // the opposite of the above
  return FileAddress{obj, address_ - obj.GetLoadBias().GetAddress()};
}
