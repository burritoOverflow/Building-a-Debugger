#include <algorithm>
#include <cxxabi.h>
#include <fcntl.h>
#include <libsdb/bit.hpp>
#include <libsdb/dwarf.hpp>
#include <libsdb/elf.hpp>
#include <libsdb/error.hpp>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

sdb::Elf::Elf(const std::filesystem::path& path) : path_(path) {
  this->path_ = path;

  if ((fd_ = open(path.c_str(), O_RDONLY)) < 0) {
    Error::SendErrno("Failed to open ELF file");
  }

  // disambiguate with the 'stat' syscall via the 'struct' declaration here
  struct stat stats;
  if (fstat(fd_, &stats) < 0) {
    Error::SendErrno("Could not retrieve ELF file stats");
  }

  // get the size of the ELF file
  this->fle_size_ = stats.st_size;

  /*
   * ELF files can be quite large, so we map the file into virtual memory of the
   * process
   */
  void* ret;

  // map the file into memory, with read-only access
  if ((ret = mmap(nullptr, this->fle_size_, PROT_READ, MAP_SHARED, fd_, 0)) ==
      MAP_FAILED) {
    close(this->fd_);
    Error::SendErrno("Could not map ELF file");
  }

  this->data_ = reinterpret_cast<std::byte*>(ret);

  // copy the header from the mapped memory to the header_ member
  std::copy(this->data_, this->data_ + sizeof(header_), AsBytes(this->header_));

  this->ParseSectionHeaders();
  this->BuildSectionMap();
  this->ParseSymbolTable();
  this->BuildSymbolMaps();

  this->dwarf_ = std::make_unique<sdb::Dwarf>(*this);
}

sdb::Elf::~Elf() {
  // unmap the memory
  if (this->data_) {
    munmap(this->data_, this->fle_size_);
  }
  // and close the file descriptor
  if (this->fd_ >= 0) {
    close(this->fd_);
  }
}

std::string_view sdb::Elf::GetString(const std::size_t index) const {
  // similar to GetSectionName, but looks up the string in the .strtab or
  // .dynstr section
  auto opt_strtab = this->GetSection(".strtab");
  if (!opt_strtab) {
    opt_strtab = this->GetSection(".dynstr");
    if (!opt_strtab) {
      return "";
    }
  }

  // return the string that begins at the given offset into the appropriate
  // section
  return {reinterpret_cast<char*>(this->data_) + opt_strtab.value()->sh_offset +
          index};
}

// get a pointer to the section header for the section name if it exists
std::optional<const Elf64_Shdr*> sdb::Elf::GetSection(
    const std::string_view name) const {
  if (this->section_map_.count(name) == 0) {
    return std::nullopt;  // section not found
  }
  return this->section_map_.at(name);
}

// Retrieve section information by section name
sdb::Span<const std::byte> sdb::Elf::GetSectionContents(
    const std::string_view name) const {
  // return a span of bytes for with the data for that section
  if (const auto sect = this->GetSection(name); sect) {
    return {this->data_ + sect.value()->sh_offset, sect.value()->sh_size};
  }
  // otherwise, return an empty span
  return {nullptr, static_cast<std::size_t>(0)};
}

const Elf64_Shdr* sdb::Elf::GetSectionContainingAddress(
    const FileAddress file_addr) const {
  if (file_addr.ElfFile() != this) {
    return nullptr;  // address not in this ELF file
  }

  for (auto& section : section_headers_) {
    if (section.sh_addr <= file_addr.GetAddress() &&
        section.sh_addr + section.sh_size > file_addr.GetAddress()) {
      return &section;  // found the section containing the address
    }
  }
  // otherwise
  return nullptr;
}

const Elf64_Shdr* sdb::Elf::GetSectionContainingAddress(
    const VirtualAddress virtual_addr) const {
  for (auto& section : section_headers_) {
    if (load_bias_ + section.sh_addr <= virtual_addr &&
        load_bias_ + section.sh_addr + section.sh_size > virtual_addr) {
      return &section;  // found the section containing the address
    }
  }
  return nullptr;
}

std::optional<sdb::FileAddress> sdb::Elf::GetSectionStartAddress(
    const std::string_view name) const {
  if (const auto section = this->GetSection(name); section) {
    // if the section exists, return the file address of that section
    return FileAddress{*this, section.value()->sh_addr};
  }
  return std::nullopt;
}

std::vector<const Elf64_Sym*> sdb::Elf::GetSymbolsByName(
    std::string_view name) const {
  auto [begin, end] = symbol_name_map_.equal_range(name);

  std::vector<const Elf64_Sym*> ret;
  std::transform(begin, end, std::back_inserter(ret),
                 [](const auto& pair) { return pair.second; });
  return ret;
}

std::optional<const Elf64_Sym*> sdb::Elf::GetSymbolAtAddress(
    FileAddress file_addr) const {
  if (file_addr.ElfFile() != this) {
    // the file address points to the wrong ELF file
    return std::nullopt;
  }

  FileAddress null_addr;
  // find the element in the map whose start address matches the given address
  const auto it = this->symbol_addr_map_.find({file_addr, null_addr});
  if (it == end(this->symbol_addr_map_)) {
    // if we don't find one, return empty optional
    return std::nullopt;
  }
  // otherwise, return the pointer to the symbol
  return it->second;
}

std::optional<const Elf64_Sym*> sdb::Elf::GetSymbolAtAddress(
    const VirtualAddress virt_addr) const {
  // defer to the above implementation
  return this->GetSymbolAtAddress(virt_addr.ToFileAddress(*this));
}

std::optional<const Elf64_Sym*> sdb::Elf::GetSymbolContainingAddress(
    FileAddress file_addr) const {
  if (file_addr.ElfFile() != this || this->symbol_addr_map_.empty()) {
    return std::nullopt;
  }

  FileAddress null_addr;

  // find the first address with a starting address greater than or equal to the
  // given one with `lower_bound`
  auto it = this->symbol_addr_map_.lower_bound({file_addr, null_addr});

  if (it != end(this->symbol_addr_map_)) {
    if (auto [key, value] = *it; key.first == file_addr) {
      return value;
    }
  }

  //  If the current iterator is the begin iterator, there is no entry preceding
  //  it.
  if (it == begin(this->symbol_addr_map_)) {
    return std::nullopt;
  }

  --it;
  // otherwise, check if the preceding entry falls within the range
  if (auto [key, value] = *it;
      key.first < file_addr && key.second > file_addr) {
    return value;
  }
  return std::nullopt;
}

std::optional<const Elf64_Sym*> sdb::Elf::GetSymbolContainingAddress(
    const VirtualAddress virt_addr) const {
  return this->GetSymbolContainingAddress(virt_addr.ToFileAddress(*this));
}

// Find the section name string table with the index given and in the ELF header
// and get the string at the given offset in that section
std::string_view sdb::Elf::GetSectionName(const std::size_t index) const {
  // use the section header string table index from the ELF header
  const auto& section = this->section_headers_[this->header_.e_shstrndx];
  return {reinterpret_cast<char*>(this->data_) + section.sh_offset + index};
}

void sdb::Elf::ParseSectionHeaders() {
  /*
   * Excerpt from the book, directly; this pertains to the first branch below:
   * Handle the special case, which occurs when a file contains many
   * sections.
   *
   * ELF reserves a range of section indices for special uses, and if
   * the number of sections reaches that limit, the file stores the number of
   * sections elsewhere. More concretely, if a file has 0xff00 sections or more,
   * it sets e_shnum to 0 and stores the number of sections in the sh_size field
   * of the first section header
   */

  auto n_headers = this->header_.e_shnum;
  if (n_headers == 0 && this->header_.e_shentsize != 0) {
    // If the file specifies the number of headers as 0, but also specifies the
    // section header size element, we've reached this limit (therefore, there
    // must be more than 0xff00 sections).

    // So, we read the sh_size field of the first section
    // header to get the real number of headers
    n_headers =
        FromBytes<Elf64_Shdr>(this->data_ + this->header_.e_shoff).sh_size;
  }

  // resize based on the number of section headers
  this->section_headers_.resize(n_headers);

  // copy the section headers from the mapped memory to the vector
  std::copy(
      this->data_ + this->header_.e_shoff,
      this->data_ + this->header_.e_shoff + sizeof(Elf64_Shdr) * n_headers,
      reinterpret_cast<std::byte*>(this->section_headers_.data()));
}

void sdb::Elf::ParseSymbolTable() {
  auto opt_symtab = this->GetSection(".symtab");
  if (!opt_symtab) {
    // check for the dynamic symbol table
    opt_symtab = this->GetSection(".dynsym");
    if (!opt_symtab) {
      // if neither exists, we can't parse the symbol table
      return;
    }
  }

  const auto symtab = *opt_symtab;

  // overall size of the symbol table (overall size / size of a single entry)
  const auto n_entries = symtab->sh_size / symtab->sh_entsize;
  this->symbol_table_.resize(n_entries);

  std::copy(this->data_ + symtab->sh_offset,
            this->data_ + symtab->sh_offset + symtab->sh_size,
            reinterpret_cast<std::byte*>(this->symbol_table_.data()));
}

// get the section name for each section from the string table and update the
// private mapping
void sdb::Elf::BuildSectionMap() {
  for (auto& section : this->section_headers_) {
    auto name                = GetSectionName(section.sh_name);
    this->section_map_[name] = &section;
  }
}

void sdb::Elf::BuildSymbolMaps() {
  for (auto& symbol : this->symbol_table_) {
    const auto mangled_name = GetString(symbol.st_name);
    int        demangle_status;

    const auto demangled_name = abi::__cxa_demangle(
        mangled_name.data(), nullptr, nullptr, &demangle_status);

    if (demangle_status == 0) {
      this->symbol_name_map_.insert({demangled_name, &symbol});
      free(demangled_name);
    }

    // add an entry regardless for the mangled name
    this->symbol_name_map_.insert({mangled_name, &symbol});

    if (symbol.st_value != 0 && symbol.st_name != 0 &&
        ELF64_ST_TYPE(symbol.st_info) != STT_TLS) {  // not thread-local storage
      const auto address_range =
          std::pair(FileAddress{*this, symbol.st_value},
                    FileAddress{*this, symbol.st_value + symbol.st_size});
      this->symbol_addr_map_.insert({address_range, &symbol});
    }
  }
}
