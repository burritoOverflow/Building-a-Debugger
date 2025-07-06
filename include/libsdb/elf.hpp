#ifndef SDB_ELF_HPP
#define SDB_ELF_HPP

#include <elf.h>
#include <filesystem>
#include <libsdb/types.hpp>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sdb {
  class Elf {
public:
    // path to an elf file on disk
    explicit Elf(const std::filesystem::path &path);
    ~Elf();

    Elf(const Elf &)           = delete;
    Elf operator=(const Elf &) = delete;

    std::filesystem::path GetPath() const { return this->path_; }

    const Elf64_Ehdr &GetHeader() const { return this->header_; }

    // retrieve a string from the string given an index
    std::string_view GetString(std::size_t index) const;

    void NotifyLoaded(const VirtualAddress address) {
      this->load_bias_ = address;
    }

    VirtualAddress GetLoadBias() const { return this->load_bias_; }

    std::optional<const Elf64_Shdr *> GetSection(std::string_view name) const;
    std::string_view                  GetSectionName(std::size_t index) const;
    Span<const std::byte> GetSectionContents(std::string_view name) const;

    // retrieve the section to which a given file address or virtual address
    // belongs
    const Elf64_Shdr *GetSectionContainingAddress(FileAddress file_addr) const;
    const Elf64_Shdr *GetSectionContainingAddress(
        VirtualAddress virtual_addr) const;

    std::optional<FileAddress> GetSectionStartAddress(
        std::string_view name) const;

    // retrieve the set of symbols that correspond to the given name
    std::vector<const Elf64_Sym *> GetSymbolsByName(
        std::string_view name) const;

    std::optional<const Elf64_Sym *> GetSymbolAtAddress(
        FileAddress file_addr) const;

    std::optional<const Elf64_Sym *> GetSymbolAtAddress(
        VirtualAddress virt_addr) const;

    std::optional<const Elf64_Sym *> GetSymbolContainingAddress(
        FileAddress file_addr) const;

    std::optional<const Elf64_Sym *> GetSymbolContainingAddress(
        VirtualAddress virt_addr) const;

private:
    void ParseSectionHeaders();
    void ParseSymbolTable();
    void BuildSectionMap();
    void BuildSymbolMaps();

    int                     fd_;
    std::filesystem::path   path_;
    std::size_t             fle_size_;
    std::byte              *data_;
    Elf64_Ehdr              header_;
    std::vector<Elf64_Shdr> section_headers_;
    std::vector<Elf64_Sym>  symbol_table_;

    // the load bias is used to translate between virtual addresses and file
    // addresses
    VirtualAddress load_bias_;

    // map from section names to section headers
    std::unordered_map<std::string_view, Elf64_Shdr *> section_map_;

    // map names to potential symbol table entries
    std::unordered_multimap<std::string_view, Elf64_Sym *> symbol_name_map_;

    struct RangeComparator {
      bool operator()(const std::pair<FileAddress, FileAddress> &lhs,
                      const std::pair<FileAddress, FileAddress> &rhs) const {
        // key comparison for the map, where first is the lower address
        return lhs.first < rhs.first;
      }
    };

    // maps a single address range to a single symbol
    std::map<std::pair<FileAddress, FileAddress>, Elf64_Sym *, RangeComparator>
        symbol_addr_map_;
  };
}  // namespace sdb

#endif  // SDB_ELF_HPP
