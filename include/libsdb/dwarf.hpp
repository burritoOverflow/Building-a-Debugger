#ifndef SDB_DWARF_HPP
#define SDB_DWARF_HPP

#include <cstdint>
#include <libsdb/detail/dwarf.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace sdb {
  class Die;
  class CompileUnit;

  class RangeList {
public:
    RangeList(const CompileUnit *compile_unit, Span<const std::byte> data,
              const FileAddress base_address) :
        compile_unit_(compile_unit), data_(data), base_address_(base_address) {}

    struct Entry {
      FileAddress low;
      FileAddress high;

      bool Contains(const FileAddress addr) const {
        return low <= addr && addr < high;
      }
    };

    // TODO: implement
    class Iterator;
    Iterator Begin() const;
    Iterator End() const;

    bool Contains(FileAddress address) const;

private:
    const CompileUnit    *compile_unit_;
    Span<const std::byte> data_;
    FileAddress           base_address_;
  };

  class RangeList::Iterator {
public:
    using value_type        = Entry;
    using reference         = const Entry &;
    using pointer           = const Entry *;
    using difference_type   = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    Iterator(const CompileUnit *compile_unit, Span<const std::byte> data,
             FileAddress base_address);

    Iterator()                            = default;
    Iterator(const Iterator &)            = default;
    Iterator &operator=(const Iterator &) = default;

    const Entry &operator*() const { return current_; }
    const Entry *operator->() const { return &current_; }

    bool operator==(const Iterator &rhs) const { return pos_ == rhs.pos_; }
    bool operator!=(const Iterator &rhs) const { return pos_ != rhs.pos_; }

    Iterator &operator++();
    Iterator  operator++(int);

private:
    const CompileUnit    *compile_unit_;
    Span<const std::byte> data_{nullptr, nullptr};
    FileAddress           base_address_;
    const std::byte      *pos_ = nullptr;
    Entry                 current_;
  };

  struct AttrSpec {
    std::uint64_t attr;
    std::uint64_t form;
  };

  // stores the values held in an abbreviation table entry
  struct Abbrev {
    std::uint64_t         code;
    std::uint64_t         tag;
    bool                  has_children;
    std::vector<AttrSpec> attr_specs;
  };

  class Elf;
  class Dwarf;

  class CompileUnit {
public:
    CompileUnit(Dwarf *parent, const Span<const std::byte> data,
                const std::size_t abbrev_offset) :
        parent_(parent), data_(data), abbrev_offset_(abbrev_offset) {}

    const Dwarf          *DwarfInfo() const { return parent_; }
    Span<const std::byte> Data() const { return data_; }

    Die GetRoot() const;

    const std::unordered_map<std::uint64_t, Abbrev> &GetAbbrevTable() const;

private:
    Dwarf *parent_;  // the dwarf object this compile-unit belongs to
    Span<const std::byte>
                data_;  // the data in the Dwarf's '.debug_info' section
    std::size_t abbrev_offset_;  // offset to the abbreviation table for this
                                 // compile-unit
  };

  class Attr {
public:
    Attr(const CompileUnit *compile_unit, const std::uint64_t type,
         const std::uint64_t form, const std::byte *location) :
        compile_unit_(compile_unit), type_(type), form_(form),
        location_(location) {}

    std::uint64_t Name() const { return type_; }
    std::uint64_t Form() const { return form_; }

    FileAddress           AsAddress() const;
    std::uint32_t         AsSectionOffset() const;
    Span<const std::byte> AsBlock() const;
    std::uint64_t         AsInt() const;
    std::string_view      AsString() const;
    Die                   AsReference() const;
    RangeList             AsRangeList() const;

private:
    const CompileUnit *compile_unit_;
    std::uint64_t      type_;  // the type of the attribute, e.g., DW_AT_name
    std::uint64_t      form_;  // the form of the attribute, e.g., DW_FORM_addr
    const std::byte   *location_;
  };

  /* Debugging Information Entry (DIE):
   * DWARF uses a series of debugging information entries (DIEs) to define a
   * low-level representation of a source program
   */
  class Die {
public:
    class ChildrenRange;

    // for null DIEs that point to the next DIE in the section
    explicit Die(const std::byte *next) : next_(next) {}

    // for non-null DIEs that store the DIE's information in the object
    Die(const std::byte *pos, const CompileUnit *compile_unit,
        const Abbrev *abbrev, std::vector<const std::byte *> attr_locations,
        const std::byte *next) :
        pos_(pos), compile_unit_(compile_unit), abbrev_(abbrev), next_(next),
        attr_locations_(std::move(attr_locations)) {}

    std::optional<std::string_view> Name() const;

    FileAddress LowPc() const;
    FileAddress HighPc() const;

    const CompileUnit *GetCompileUnit() const { return compile_unit_; }
    const Abbrev      *GetAbbrevEntry() const { return abbrev_; }
    const std::byte   *GetPosition() const { return pos_; }
    const std::byte   *GetNext() const { return next_; }

    /* NOTE:
     * Every DIE has a set of attributes, each with a type, a form, and a value.
     * A DIE can’t have multiple attributes of the same type defined;
     * it can’t have two different names, two different locations,
     * two different siblings, etc.
     *
     * Each of the following takes an attribute identifier, like
     * 'DW_AT_sibling.'
     */
    bool Contains(
        std::uint64_t attribute) const;  // does the DIE contain
                                         // the attribute of this type?

    Attr operator[](std::uint64_t attribute) const;  // get the value for the
                                                     // attribute of this type

    ChildrenRange Children() const;

    // checks whether the given file address is within the ranges of this DIE
    bool ContainsAddress(FileAddress address) const;

private:
    const std::byte               *pos_          = nullptr;
    const CompileUnit             *compile_unit_ = nullptr;
    const Abbrev                  *abbrev_       = nullptr;
    const std::byte               *next_         = nullptr;
    std::vector<const std::byte *> attr_locations_;
  };

  class Dwarf {
public:
    explicit Dwarf(const Elf &parent);
    const Elf *ElfFile() const { return elf_; }

    const std::unordered_map<std::uint64_t, Abbrev> &GetAbbrevTable(
        std::size_t offset);

    const std::vector<std::unique_ptr<CompileUnit>> &CompileUnits() const {
      return compile_units_;
    }

    const CompileUnit *CompileUnitContainingAddress(FileAddress address) const;

    std::optional<Die> FunctionContainingAddress(FileAddress address) const;

    std::vector<Die> FindFunctions(std::string name) const;

private:
    void Index() const;
    void IndexDie(const Die &current) const;

    const Elf *elf_;

    struct IndexEntry {
      const CompileUnit *cu;
      const std::byte   *pos;
    };

    mutable std::unordered_multimap<std::string, IndexEntry> function_index_;

    std::unordered_map<std::size_t, std::unordered_map<std::uint64_t, Abbrev>>
        abbrev_tables_;

    std::vector<std::unique_ptr<CompileUnit>> compile_units_;
  };

  class Die::ChildrenRange {
public:
    explicit ChildrenRange(const Die die) : die_(std::move(die)) {}

    class Iterator {
  public:
      using value_type      = Die;
      using reference       = const Die &;
      using pointer         = const Die *;
      using difference_type = std::ptrdiff_t;
      using iterator_category =
          std::forward_iterator_tag;  // multipass iterators

      Iterator()                            = default;
      Iterator(const Iterator &)            = default;
      Iterator &operator=(const Iterator &) = default;

      explicit Iterator(const Die &die);

      const Die &operator*() const { return *this->die_; }
      const Die *operator->() const { return &this->die_.value(); }

      Iterator &operator++();     // pre-increment operator
      Iterator  operator++(int);  // post-increment operator

      bool operator==(const Iterator &rhs) const;
      bool operator!=(const Iterator &rhs) const { return !(*this == rhs); }

  private:
      std::optional<Die> die_;
    };  // end Iterator

    Iterator begin() const {
      if (die_.abbrev_->has_children) {
        return Iterator{die_};
      }
      return end();
    }

    Iterator end() const { return Iterator(); }

private:
    Die die_;
  };
}  // namespace sdb

#endif  // SDB_DWARF_HPP
