#include <iostream>
#include <libsdb/bit.hpp>
#include <libsdb/registers.hpp>

#include "libsdb/process.hpp"

namespace {
  template <class T>
  sdb::byte128 Widen(const sdb::RegisterInfo& info, T t) {
    // when floating-point cast it to the  widest relevant fp type
    // before casting it again to a byte128 type
    if constexpr (std::is_floating_point_v<T>) {
      if (info.format == sdb::RegisterFormat::DOUBLE_FLOAT) {
        return sdb::ToByte128(static_cast<double>(t));
      }

      if (info.format == sdb::RegisterFormat::LONG_DOUBLE) {
        return sdb::ToByte128(static_cast<long double>(t));
      }
    } else if constexpr (std::is_signed_v<T>) {
      // if signed integer, we sign-extend it to the size of the register,
      // before casting to byte128
      if (info.format == sdb::RegisterFormat::UINT) {
        switch (info.size) {
          case 2:
            return sdb::ToByte128(static_cast<std::int16_t>(t));
          case 4:
            return sdb::ToByte128(static_cast<std::int32_t>(t));
          case 8:
            return sdb::ToByte128(static_cast<std::int64_t>(t));
        }
      }
    }
    // otherwise, unsigned integer
    // so, for 32-bit, for example, we can copy over the 32 bits
    // and write 0 s into the rest of the register
    return sdb::ToByte128(t);
  }
}  // namespace

sdb::Registers::value sdb::Registers::Read(const RegisterInfo& info) const {
  const auto bytes = AsBytes(data_);

  if (info.format == RegisterFormat::UINT) {
    switch (info.size) {
      case 1:
        return FromBytes<std::uint8_t>(bytes + info.offset);
      case 2:
        return FromBytes<std::uint16_t>(bytes + info.offset);
      case 4:
        return FromBytes<std::uint32_t>(bytes + info.offset);
      case 8:
        return FromBytes<std::uint64_t>(bytes + info.offset);
      default:
        Error::Send("Unexpected register size");
    }
  }
  if (info.format == RegisterFormat::DOUBLE_FLOAT) {
    return FromBytes<double>(bytes + info.offset);
  }
  if (info.format == RegisterFormat::LONG_DOUBLE) {
    return FromBytes<long double>(bytes + info.offset);
  }
  if (info.format == RegisterFormat::VECTOR and info.size == 8) {
    return FromBytes<byte64>(bytes + info.offset);
  }
  return FromBytes<byte128>(bytes + info.offset);
}

void sdb::Registers::Write(const RegisterInfo& info, value value) {
  auto bytes = AsBytes(data_);

  std::visit(
      [&](auto& v)
      {
        if (sizeof(v) <= info.size) {
          auto wide      = Widen(info, v);
          auto val_bytes = AsBytes(wide);
          // val_bytes + info.size so that the widened value is written
          std::copy(val_bytes, val_bytes + info.size, bytes + info.offset);
        } else {
          std::cerr << "sdb::Register::Write: called with "
                       "mismatched register and value sizes\n";
          std::terminate();
        }
      },
      value);

  if (info.type == RegisterType::FPR) {
    // PTRACE_POKEUSER and PTRACE_PEEKUSER don’t support writing and
    // reading from the x87 area on x64
    // we'll write to all FPRs at once.
    proc_->WriteFprs(data_.i387);
  } else {  // otherwise, we're writing to a single GPR or DR
    // Note: PTRACE_PEEKUSER and PTRACE_POKEUSER require the offset to be
    // aligned to 8 bytes so, set the lowest 3 bits to 0, forcing 8 byte
    // alignment
    const auto aligned_offset = info.offset & ~0b111;
    proc_->WriteUserArea(info.offset,
                         FromBytes<std::uint64_t>(bytes + aligned_offset));
  }
}
