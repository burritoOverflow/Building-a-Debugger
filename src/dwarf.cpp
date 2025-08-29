#include <algorithm>
#include <libsdb/bit.hpp>
#include <libsdb/dwarf.hpp>
#include <libsdb/elf.hpp>
#include <libsdb/error.hpp>
#include <libsdb/types.hpp>

namespace {
  /*
   * Cursor points to a location in the DWARF information, allowing
   * for easy access to information from that location
   *
   * Handles the parsing of the data and advancement of the location being
   * pointed to.
   */
  class Cursor {
public:
    explicit Cursor(const sdb::Span<const std::byte> data) :
        data_(data), pos_(data.begin()) {}

    // helpers to walk the cursor forward
    Cursor operator++() {
      ++pos_;
      return *this;
    }

    Cursor operator+=(const std::size_t size) {
      pos_ += size;
      return *this;
    }

    // parse fixed-width integers
    template <class T>
    T FixedInt() {
      const auto t = sdb::FromBytes<T>(this->pos_);
      this->pos_ += sizeof(T);
      return t;
    }

    std::uint8_t  u8() { return this->FixedInt<std::uint8_t>(); }
    std::uint16_t u16() { return this->FixedInt<std::uint16_t>(); }
    std::uint32_t u32() { return this->FixedInt<std::uint32_t>(); }
    std::uint64_t u64() { return this->FixedInt<std::uint64_t>(); }
    std::int8_t   s8() { return this->FixedInt<std::int8_t>(); }
    std::int16_t  s16() { return this->FixedInt<std::int16_t>(); }
    std::int32_t  s32() { return this->FixedInt<std::int32_t>(); }
    std::int64_t  s64() { return this->FixedInt<std::int64_t>(); }

    std::string_view String() {
      // find the first 0 byte between the current position and the end of the
      // data
      const auto null_terminator =
          std::find(this->pos_, this->data_.end(), std::byte{0});

      // size is the distance to the null terminator
      const std::string_view ret(reinterpret_cast<const char *>(this->pos_),
                                 null_terminator - this->pos_);
      this->pos_ = null_terminator + 1;
      return ret;
    }

    // From the book, verbatim:
    // "Encoding these values requires splitting the integer into
    // groups of 7 bits, then adding a bit to the front of each group.
    //
    // To reverse this process, we read the value one byte at a time, remove the
    // first bit of the byte, then shift the remaining 7 bits into their correct
    // place. We stop when we get to a byte that starts with a 0."
    std::uint64_t uleb128() {
      std::uint64_t result = 0;  // result of the decoding
      int shift = 0;  // amount by which to shift the next byte to the left
      std::uint8_t byte = 0;  // current byte we're looking at

      do {
        byte              = u8();
        const auto masked = static_cast<uint64_t>(byte & 0x7f);
        // set the bits
        result |= (masked << shift);
        // shift the next 7 bits into position
        shift += 7;
      } while ((byte & 0x80) !=
               0);  // end when the topmost bit of the read byte is 0
      return result;
    }

    std::int64_t sleb128() {
      // use uints here to avoid unexpected behavior (i.e left-shifting a
      // negative signed integer)
      std::uint64_t result = 0;
      int           shift  = 0;
      std::uint8_t  byte   = 0;

      // As above.
      do {
        byte              = u8();
        const auto masked = static_cast<uint64_t>(byte & 0x7f);
        result |= (masked << shift);
        shift += 7;
      } while ((byte & 0x80) != 0);
      // Key differences are below.

      /*
       * First, we check whether we filled the result integer by comparing
       * shift to the size of the result storage.
       * We determine whether the number should be negative by checking whether
       * the last byte read has a 1 in its second-highest position.
       */
      if ((shift < sizeof(result) * 8) and
          (byte & 0x40)) {  // NOTE: the sign bit of byte is the second
                            // high-order bit (0x40)
        // if so, we fill the remaining high bits of the result by bit-flipping
        // 0 to obtain an integer with all bits set and then left-shifting the
        // unnecessary ones off the end
        result |= (~static_cast<std::int64_t>(0) << shift);
      }

      return result;
    }

    const std::byte *Position() const { return pos_; }

    // has all the data been read?
    bool IsFinished() const { return pos_ >= data_.end(); }

    void SkipForm(const std::uint64_t form) {
      switch (form) {
        case DW_FORM_flag_present:
          break;  // indicates presence; this value requires no bytes of storage
                  // (implicit)

        case DW_FORM_data1:  // integer of the specified byte width (1 byte,
                             // here)
        case DW_FORM_ref1:   // 1-byte offset from the start of the current
                             // compile unit header
        case DW_FORM_flag:   // encoded as a single byte (explicitly)
          this->pos_ += 1;
          break;

          // as above, but two bytes
        case DW_FORM_data2:
        case DW_FORM_ref2:
          this->pos_ += 2;
          break;

          // as above, but four bytes
        case DW_FORM_data4:
        case DW_FORM_ref4:
        case DW_FORM_ref_addr:    // a 4-byte integer offset to anywhere in the
                                  // .debug_info section
        case DW_FORM_sec_offset:  // a 4-byte offset into a section
                                  // other than .debug_info or .debug_str
        case DW_FORM_strp:        // a 4-byte integer offset into the .debug_str
          // section
          this->pos_ += 4;
          break;

        case DW_FORM_data8:  // an 8-byte integer
        case DW_FORM_addr:   // integer of the byte width for addresses on the
                             //  system (here, 8 for x64).
          this->pos_ += 8;
          break;

          // the following are integers encoded in ULEB128 or SLEB128,
          // respectively; parse either and throw away the result
        case DW_FORM_sdata:
          sleb128();
          break;

        case DW_FORM_udata:
        case DW_FORM_ref_udata:
          uleb128();
          break;

          // cover the explicitly sized forms (where the size is encoded in the
          // attribute value itself)
          // this covers data block forms (DW_FORM_block and
          // DW_FORM_block{1,2,4})
        case DW_FORM_block1:
          this->pos_ += this->u8();
          break;
        case DW_FORM_block2:
          this->pos_ += this->u16();
          break;
        case DW_FORM_block4:
          this->pos_ += this->u32();
          break;

        case DW_FORM_block:
        case DW_FORM_exprloc:
          this->pos_ += this->uleb128();
          break;

        case DW_FORM_string:
          // sequence of non-null bytes terminated by a null byte
          while (!this->IsFinished() &&
                 *this->pos_ != static_cast<std::byte>(0)) {
            ++this->pos_;
          }
          ++this->pos_;  // skip the null terminator
          break;

          // DW_FORM_indirect is a value encoded as a ULEB128 that gives its
          // actual form, followed by an attribute encoded in that form
        case DW_FORM_indirect:
          // read the form from the cursor and recursively invoke with the
          // result
          SkipForm(uleb128());
          break;

        default:
          sdb::Error::Send("Unrecognized DWARF form encountered");
      }
    }

private:
    sdb::Span<const std::byte> data_;  // current data range
    const std::byte           *pos_;   // cursor's current position
  };

  std::unordered_map<std::uint64_t, sdb::Abbrev> ParseAbbrevTable(
      const sdb::Elf &elf, const std::size_t offset) {
    Cursor cursor(elf.GetSectionContents(
        ".debug_abbrev"));  // location of the abbreviation table
    cursor += offset;

    std::unordered_map<std::uint64_t, sdb::Abbrev> abbrev_table;
    std::uint64_t                                  code = 0;
    do {
      code                    = cursor.uleb128();  // read the entry code
      const auto tag          = cursor.uleb128();
      const auto has_children = static_cast<bool>(
          cursor.u8());  // 1-byte unsigned int for the children flag (1 or 0)

      std::vector<sdb::AttrSpec> attr_specs;
      std::uint64_t              attr = 0;
      do {
        attr            = cursor.uleb128();
        const auto form = cursor.uleb128();
        if (attr != 0) {
          attr_specs.push_back(
              sdb::AttrSpec{attr, form});  // add to the list of attributes
        }
      } while (attr != 0);
      if (code != 0) {
        abbrev_table.emplace(
            code, sdb::Abbrev{code, tag, has_children, std::move(attr_specs)});
      }


    } while (code != 0);  // until the entry code is 0

    return abbrev_table;
  }

  // parse the compile unit header
  std::unique_ptr<sdb::CompileUnit> ParseCompileUnit(
      sdb::Dwarf &dwarf, [[maybe_unused]] const sdb::Elf &obj, Cursor cursor) {
    const auto start     = cursor.Position();
    auto       size      = cursor.u32();
    const auto version   = cursor.u16();
    const auto abbrev    = cursor.u32();  // offset for the abbreviation table
    const auto addr_size = cursor.u8();

    if (size == 0xffffffff) {
      sdb::Error::Send("Only DWARF32 is supported.");
    }
    if (version != 4) {
      sdb::Error::Send("Only DWARF version 4 is supported.");
    }
    if (addr_size != 8) {
      sdb::Error::Send("Invalid address size for DWARF");
    }

    size += sizeof(std::uint32_t);

    const sdb::Span<const std::byte> data = {start, size};
    return std::make_unique<sdb::CompileUnit>(&dwarf, data, abbrev);
  }

  std::vector<std::unique_ptr<sdb::CompileUnit>> ParseCompileUnits(
      sdb::Dwarf &dwarf, const sdb::Elf &obj) {
    const auto debug_info = obj.GetSectionContents(".debug_info");
    Cursor     cursor(debug_info);

    std::vector<std::unique_ptr<sdb::CompileUnit>> compile_units;

    while (!cursor.IsFinished()) {
      auto unit = ParseCompileUnit(dwarf, obj, cursor);
      cursor += unit->Data().Size();
      compile_units.push_back(std::move(unit));
    }

    return compile_units;
  }

  sdb::Die ParseDie([[maybe_unused]] const sdb::CompileUnit &compile_unit,
                    Cursor                                   cursor) {
    /*
     * DWARF encodes a DIE as a ULEB128 representing its abbreviation code,
     * followed by a list of attribute values.
     */
    const auto position    = cursor.Position();
    const auto abbrev_code = cursor.uleb128();

    // null DIE; the next DIE starts at the position currently pointed to by the
    // cursor
    if (abbrev_code == 0) {
      const auto next = cursor.Position();
      return sdb::Die{next};
    }

    // otherwise, we grab the abbrev table for this compile unit
    auto &abbrev_table = compile_unit.GetAbbrevTable();
    // look up the nested table from the abbrev code
    auto &abbrev = abbrev_table.at(abbrev_code);

    // we know it's the same size as the attribution specification vector inside
    // the abbreviation entry, so reserve this size
    std::vector<const std::byte *> attr_locations;
    attr_locations.reserve(abbrev.attr_specs.size());

    for (const auto &[attr, form] : abbrev.attr_specs) {
      attr_locations.push_back(cursor.Position());
      cursor.SkipForm(form);
    }

    const auto next = cursor.Position();
    return sdb::Die{position, &compile_unit, &abbrev, std::move(attr_locations),
                    next};
  }

}  // namespace

sdb::RangeList::Iterator sdb::RangeList::Begin() const {
  return Iterator{this->compile_unit_, this->data_, this->base_address_};
}

sdb::RangeList::Iterator sdb::RangeList::End() const { return {}; }

bool sdb::RangeList::Contains(const FileAddress address) const {
  return std::any_of(this->Begin(), this->End(),
                     [=](auto &e) { return e.Contains(address); });
}

sdb::RangeList::Iterator::Iterator(const CompileUnit    *compile_unit,
                                   Span<const std::byte> data,
                                   const FileAddress     base_address) :
    compile_unit_(compile_unit), data_(data), base_address_(base_address),
    pos_(data.begin()) {
  // prime the first element of the range
  ++(*this);
}

sdb::RangeList::Iterator &sdb::RangeList::Iterator::operator++() {
  const auto     elf = this->compile_unit_->DwarfInfo()->ElfFile();
  constexpr auto base_address_flag = -static_cast<std::int64_t>(0);

  Cursor cursor({this->pos_, this->data_.end()});

  while (true) {
    current_.low  = FileAddress{*elf, cursor.u64()};
    current_.high = FileAddress{*elf, cursor.u64()};

    if (current_.low.GetAddress() == base_address_flag) {
      //  If the first of the pair is the base address entry flag,
      //  we set the base address to the value of the second integer.
      this->base_address_ = current_.high;
    } else if (current_.low.GetAddress() == 0 and
               current_.high.GetAddress() == 0) {
      // if both integers are 0, we have an end-of-list indicator, set pos to
      // null to indicate iteration is complete
      this->pos_ = nullptr;
      break;
    } else {
      // otherwise, entry is regular so save the current Cursor position. incr
      // the current address by the base addr to calculate the correct values
      // for this entry and exit loop
      this->pos_ = cursor.Position();
      current_.low += this->base_address_.GetAddress();
      current_.high += this->base_address_.GetAddress();
      break;
    }
  }
  return *this;
}

sdb::RangeList::Iterator sdb::RangeList::Iterator::operator++(int) {
  auto tmp = *this;
  ++(*this);
  return tmp;
}

// DIE trees for each program begin at the root nodes for each CU.
// obtain the root DIE for this compile unit
sdb::Die sdb::CompileUnit::GetRoot() const {
  constexpr std::size_t header_size = 11;
  const Cursor cursor({this->data_.begin() + header_size, this->data_.end()});
  return ParseDie(*this, cursor);
}

const std::unordered_map<std::uint64_t, sdb::Abbrev> &
sdb::CompileUnit::GetAbbrevTable() const {
  return this->parent_->GetAbbrevTable(this->abbrev_offset_);
}

// we're limiting ourselves to x64 here, so we can just read a single 64-bit
// integer from the start of the attribute bytes and return that as a
// FileAddress
sdb::FileAddress sdb::Attr::AsAddress() const {
  if (this->form_ != DW_FORM_addr) {
    // addresses must be of this form.
    Error::Send("Invalid address type");
  }

  Cursor cursor({this->location_, this->compile_unit_->Data().end()});

  const auto elf = this->compile_unit_->DwarfInfo()->ElfFile();

  // NOTE: this address is a file address in the ELF file to which the DWARF
  // information belongs
  return FileAddress{*elf, cursor.u64()};
}

std::uint32_t sdb::Attr::AsSectionOffset() const {
  if (this->form_ != DW_FORM_sec_offset) {
    Error::Send("Invalid offset type");
  }

  Cursor cursor({this->location_, this->compile_unit_->Data().end()});
  return cursor.u32();
}

sdb::Span<const std::byte> sdb::Attr::AsBlock() const {
  // DWARF encodes as a size followed by the data
  std::size_t size;
  Cursor      cursor({this->location_, this->compile_unit_->Data().end()});

  switch (this->form_) {
    case DW_FORM_block1:
      size = cursor.u8();
      break;
    case DW_FORM_block2:
      size = cursor.u16();
      break;
    case DW_FORM_block4:
      size = cursor.u32();
      break;
    case DW_FORM_block:
      size = cursor.uleb128();
      break;
    default:
      Error::Send("Invalid block type");
  }

  return {cursor.Position(), size};
}

std::uint64_t sdb::Attr::AsInt() const {
  Cursor cursor({this->location_, this->compile_unit_->Data().end()});
  // check the form for the size of the integer to parse
  switch (this->form_) {
    case DW_FORM_data1:
      return cursor.u8();
    case DW_FORM_data2:
      return cursor.u16();
    case DW_FORM_data4:
      return cursor.u32();
    case DW_FORM_data8:
      return cursor.u64();
    case DW_FORM_udata:
      return cursor.uleb128();
    default:
      Error::Send("Invalid integer type");
  }
}

std::string_view sdb::Attr::AsString() const {
  Cursor cursor({this->location_, this->compile_unit_->Data().end()});
  switch (this->form_) {
    case DW_FORM_string:
      // string embedded in the DIE, parse and return it
      return cursor.String();
      // a 4-byte offset into the .debug_str section that indicates where the
      // string resides
    case DW_FORM_strp:
      {
        const auto offset = cursor.u32();

        // get the contents of that section
        const auto stab =
            this->compile_unit_->DwarfInfo()->ElfFile()->GetSectionContents(
                ".debug_str");

        // parse the string from there
        Cursor stab_cursor({stab.begin() + offset, stab.end()});
        return stab_cursor.String();
      }
    default:
      Error::Send("Invalid string type");
  }
}

// parse an offset of the correct size for the attribute's form, then parse
// the DIE that exists at that offset from the start of the compile unit
sdb::Die sdb::Attr::AsReference() const {
  std::size_t offset;
  Cursor      cursor({this->location_, this->compile_unit_->Data().end()});

  // first, the fixed length forms for 1,2, 4, and 8 byte offsets, respectively
  switch (this->form_) {
    case DW_FORM_ref1:
      offset = cursor.u8();
      break;
    case DW_FORM_ref2:
      offset = cursor.u16();
      break;
    case DW_FORM_ref4:
      offset = cursor.u32();
      break;
    case DW_FORM_ref8:
      offset = cursor.u64();
      break;
    case DW_FORM_ref_udata:
      // an unsigned variable length offset encoded form that uses unsigned
      // LEB128 numbers
      offset = cursor.uleb128();
      break;
    case DW_FORM_ref_addr:
      {
        offset = cursor.u32();

        const auto section_contents =
            this->compile_unit_->DwarfInfo()->ElfFile()->GetSectionContents(
                ".debug_info");

        // calculate the position of the referenced DIE
        const auto die_position = section_contents.begin() + offset;

        //  We need to know to which compile unit it belongs, so we look through
        //  all the compile units contained in the debug information for a
        //  compile unit whose data range contains the DIE we’re looking for
        const auto compile_unit_finder = [=](auto &cu)
        {
          // is the DIE position within the compile unit's data range?
          return cu->Data().begin() <= die_position and
                 cu->Data().end() > die_position;
        };

        auto &compile_units = this->compile_unit_->DwarfInfo()->CompileUnits();

        // find the correct compile unit
        const auto compile_unit_for_offset = std::find_if(
            begin(compile_units), end(compile_units), compile_unit_finder);

        const Cursor ref_cursor(
            {die_position, compile_unit_for_offset->get()->Data().end()});

        return ParseDie(**compile_unit_for_offset, ref_cursor);
      }
    default:
      Error::Send("Invalid reference type");
  }

  const Cursor ref_cursor({this->compile_unit_->Data().begin() + offset,
                           this->compile_unit_->Data().end()});
  return ParseDie(*this->compile_unit_, ref_cursor);
}

sdb::RangeList sdb::Attr::AsRangeList() const {
  // DWARF encodes range list attributes as offsets into the .debug_ranges
  // section.
  const auto section =
      this->compile_unit_->DwarfInfo()->ElfFile()->GetSectionContents(
          ".debug_ranges");

  const auto            offset = this->AsSectionOffset();
  Span<const std::byte> data{section.begin() + offset, section.end()};

  const auto root = this->compile_unit_->GetRoot();

  //  We need the initial base address, if there is one.
  //  We can find this value in the DW_AT_low_pc attribute, if present.
  //  If the DW_AT_low_pc attribute exists in the root compile unit DIE, we use
  //  it; otherwise, use an empty address
  FileAddress base_address = root.Contains(DW_AT_low_pc)
                                 ? root[DW_AT_low_pc].AsAddress()
                                 : FileAddress{};

  return {this->compile_unit_, data, base_address};
}

const std::unordered_map<std::uint64_t, sdb::Abbrev> &
sdb::Dwarf::GetAbbrevTable(std::size_t offset) {
  if (!this->abbrev_tables_.count(offset)) {
    // not yet parsed, do so now and store the result
    this->abbrev_tables_.emplace(offset, ParseAbbrevTable(*this->elf_, offset));
  }
  // return the previously parsed table
  return this->abbrev_tables_.at(offset);
}

const sdb::CompileUnit *sdb::Dwarf::CompileUnitContainingAddress(
    const FileAddress address) const {
  for (auto &cu : this->compile_units_) {
    // iterate over the compile units for this Dwarf object
    if (cu->GetRoot().ContainsAddress(address)) {
      //  if we find one whose root DIE contains the given address return a
      //  pointer to it
      return cu.get();
    }
  }
  return nullptr;
}

std::optional<sdb::Die> sdb::Dwarf::FunctionContainingAddress(
    const FileAddress address) const {
  // Index the dwarf information to ensure that `function_index_` is populated
  this->Index();

  for (auto &[name, entry] : this->function_index_) {
    const Cursor cursor({entry.pos, entry.cu->Data().end()});

    if (auto d = ParseDie(*entry.cu, cursor);
        d.ContainsAddress(address) &&
        d.GetAbbrevEntry()->tag == DW_TAG_subprogram) {
      return d;
    }
  }
  // this may occur for example if the code at that address belongs to some
  // other shared library.
  return std::nullopt;
}

std::vector<sdb::Die> sdb::Dwarf::FindFunctions(std::string name) const {
  this->Index();

  std::vector<Die> found;

  // get a pair of iterators to the range of entries matching the key
  auto [begin, end] = this->function_index_.equal_range(name);

  // push the results into the found vector
  std::transform(begin, end, std::back_inserter(found),
                 [](auto &pair)
                 {
                   auto [name, entry] = pair;
                   Cursor cursor({entry.pos, entry.cu->Data().end()});
                   return ParseDie(*entry.cu, cursor);
                 });

  return found;
}

void sdb::Dwarf::Index() const {
  // check that the dwarf information is only indexed once
  if (!this->function_index_.empty()) {
    return;
  }
  for (auto &cu : this->compile_units_) {
    this->IndexDie(cu->GetRoot());
  }
}

void sdb::Dwarf::IndexDie(const Die &current) const {
  /*
   * This function should add the given DIE to the function index so long as it
   * has address range data, then recursively index all of that DIE’s children.
   */

  // a DIE has an address range if it has either a DW_AT_low_pc attribute or a
  // DW_AT_ranges attribute
  const bool has_range =
      current.Contains(DW_AT_low_pc) or current.Contains(DW_AT_ranges);

  // DWARF specifies functions with either the DW_TAG_subprogram tag or,
  // if the DIE represents a function whose body the compiler has copied from
  // elsewhere, the DW_TAG_inlined_subroutine tag
  const bool is_function =
      current.GetAbbrevEntry()->tag == DW_TAG_subprogram or
      current.GetAbbrevEntry()->tag == DW_TAG_inlined_subroutine;

  if (has_range and is_function) {
    if (const auto name = current.Name(); name) {
      IndexEntry entry{current.GetCompileUnit(), current.GetPosition()};
      this->function_index_.emplace(*name, entry);
    }
  }

  // recursively index children
  for (auto &child : current.Children()) {
    this->IndexDie(child);
  }
}

sdb::Die::ChildrenRange::Iterator::Iterator(const Die &die) {
  const Cursor next_cursor({die.next_, die.compile_unit_->Data().end()});
  this->die_ = ParseDie(*die.compile_unit_, next_cursor);
}

// pre-increment operator
sdb::Die::ChildrenRange::Iterator &
sdb::Die::ChildrenRange::Iterator::operator++() {
  if (!this->die_.has_value() or !this->die_->abbrev_) {
    return *this;
  }

  // DIE is valid and doesn't have children
  if (!this->die_->abbrev_->has_children) {
    const Cursor next_cursor(
        {this->die_->next_, this->die_->compile_unit_->Data().end()});
    this->die_ = ParseDie(*this->die_->compile_unit_, next_cursor);
  } else if (this->die_->Contains(DW_AT_sibling)) {
    // skip directly to the sibling DIE if it exists
    this->die_ = this->die_.value()[DW_AT_sibling].AsReference();
  } else {  // DIE has children
    Iterator sub_children_iter(*this->die_);

    // advance the iterator until we find a null entry
    while (sub_children_iter->abbrev_) {
      ++sub_children_iter;
    }

    // the next DIE after the null entry is the sibling we're looking for, so
    // parse that.
    const Cursor next_cursor(
        {sub_children_iter->next_, this->die_->compile_unit_->Data().end()});
    this->die_ = ParseDie(*this->die_->compile_unit_, next_cursor);
  }

  return *this;
}

sdb::Die::ChildrenRange::Iterator sdb::Die::ChildrenRange::Iterator::operator++(
    int) {
  auto tmp = *this;
  ++(*this);
  return tmp;
}

bool sdb::Die::ChildrenRange::Iterator::operator==(const Iterator &rhs) const {
  // an iterator is null if there is no DIE stored or if the DIE has an
  // abbreviation code of 0
  const auto lhs_null =
      !this->die_.has_value() or !this->die_->GetAbbrevEntry();
  const auto rhs_null = !rhs.die_.has_value() or !rhs.die_->GetAbbrevEntry();

  if (lhs_null and rhs_null) {
    // eq if both null
    return true;
  }

  if (lhs_null or rhs_null) {
    return false;
  }

  // both are non-null, so we compare the DIEs abbreviation codes and next
  // pointers for equality
  return this->die_->abbrev_ == rhs.die_->abbrev_ and
         this->die_->GetNext() == rhs.die_->GetNext();
}

std::optional<std::string_view> sdb::Die::Name() const {
  if (this->Contains(DW_AT_name)) {
    return (*this)[DW_AT_name].AsString();
  }

  /*
   * From the Book:
   * If it has a DW_AT_specification or a DW_AT_abstract_origin attribute, we
   * resolve that attribute as a reference to another DIE and then call .name on
   * it. We call .name rather than just grabbing the DW_AT_name attribute on the
   * result to account for chains of references (for example, out-of-line
   * definitions that were inlined).
   */
  if (this->Contains(DW_AT_specification)) {
    return (*this)[DW_AT_specification].AsReference().Name();
  }

  if (this->Contains(DW_AT_abstract_origin)) {
    return (*this)[DW_AT_abstract_origin].AsReference().Name();
  }

  return std::nullopt;
}

sdb::FileAddress sdb::Die::LowPc() const {
  if (this->Contains(DW_AT_ranges)) {
    const auto first_entry = (*this)[DW_AT_ranges].AsRangeList().Begin();
    return first_entry->low;
  }

  if (this->Contains(DW_AT_low_pc)) {
    return (*this)[DW_AT_low_pc].AsAddress();
  }

  Error::Send("DIE does not have low PC");
}

sdb::FileAddress sdb::Die::HighPc() const {
  if (this->Contains(DW_AT_ranges)) {
    const auto ranges = (*this)[DW_AT_ranges].AsRangeList();
    auto       it     = ranges.Begin();
    while (std::next(it) != ranges.End()) {
      ++it;
    }
    return it->high;
  }

  if (this->Contains(DW_AT_high_pc)) {
    const auto  attr = (*this)[DW_AT_high_pc];
    FileAddress addr;
    if (attr.Form() == DW_FORM_addr) {
      return attr.AsAddress();
    } else {
      return this->LowPc() + attr.AsInt();
    }
  }
  Error::Send("DIE does not have high PC");
}

// look through the DIE's attribute specifications in the abbreviation entry
// and check if there exists one for matching the given attribute type
bool sdb::Die::Contains(std::uint64_t attribute) const {
  const auto &specs = this->abbrev_->attr_specs;
  return std::find_if(begin(specs), end(specs), [=](auto spec)
                      { return spec.attr == attribute; }) != end(specs);
}

sdb::Attr sdb::Die::operator[](const std::uint64_t attribute) const {
  const auto &specs = this->abbrev_->attr_specs;

  for (std::size_t i = 0; i < specs.size(); ++i) {
    // find a match for the attribute we're looking for
    if (specs[i].attr == attribute) {
      return {this->compile_unit_, specs[i].attr, specs[i].form,
              this->attr_locations_[i]};
    }
  }

  Error::Send("Attribute not found");
}

sdb::Die::ChildrenRange sdb::Die::Children() const {
  return ChildrenRange(*this);
}

bool sdb::Die::ContainsAddress(FileAddress address) const {
  if (address.ElfFile() != this->compile_unit_->DwarfInfo()->ElfFile()) {
    return false;
  }

  if (Contains(DW_AT_ranges)) {
    return (*this)[DW_AT_ranges].AsRangeList().Contains(address);
  }

  if (Contains(DW_AT_low_pc)) {
    return this->LowPc() <= address and this->HighPc() > address;
  }

  return false;
}

sdb::Dwarf::Dwarf(const Elf &parent) : elf_(&parent) {
  this->compile_units_ = ParseCompileUnits(*this, parent);
}
