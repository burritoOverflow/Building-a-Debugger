#include <libsdb/breakpoint_site.hpp>
#include <libsdb/process.hpp>
#include <sys/ptrace.h>

namespace {
  auto GetNextID() {
    static sdb::BreakpointSite::id_type id = 0;
    return ++id;
  }
}  // namespace

sdb::BreakpointSite::BreakpointSite(Process&             process,
                                    const VirtualAddress address,
                                    const bool           is_hardware,
                                    const bool           is_internal) :
    process_{&process}, address_{address}, is_enabled_{false}, saved_data_{},
    is_hardware_{is_hardware}, is_internal_{is_internal} {
  // site id for internal breakpoints is -1
  this->id_ = this->is_internal_ ? -1 : GetNextID();
}

void sdb::BreakpointSite::Enable() {
  if (this->is_enabled_) {
    return;
  }

  if (this->is_hardware_) {
    this->hardware_register_index_ =
        this->process_->SetHardwareBreakpoint(this->id_, this->address_);
  } else {
    errno = 0;
    // Read a word at the address in the tracee's memory
    const std::uint64_t data = ptrace(PTRACE_PEEKDATA, this->process_->GetPid(),
                                      this->address_, nullptr);
    if (errno != 0) {
      Error::SendErrno("Enabling breakpoint site failed");
    }

    /*
     * we received 64-bits of data; we only need the first 8 bits, so we bitwise
     * and the data with '0xff' to get the last byte of the data. ("masking out"
     * all the higher-order bits) Example: (0xe5894855fa1e0ff3 & 0xff) will give
     * us '0xf3'
     */
    this->saved_data_ = static_cast<std::byte>(data & 0xff);

    // int3 instruction
    constexpr std::uint64_t int3 = 0xcc;

    // We'll need to replace the bits we just saved with '0xcc'.
    // First, zero out the bits with a bitwise AND with ~0xff then bitwise OR
    // with '0xcc' to set the correct bits

    // now, for replacement, the bitwise AND with ~0xff will zero out the last 8
    // bits and then a bitwise or with '0xcc' will set the last 8 bits to '0xcc'
    const std::uint64_t data_with_int3 = ((data & ~0xff) | int3);

    // Copy the word data to offset addr in the tracee's memory
    if (ptrace(PTRACE_POKEDATA, this->process_->GetPid(), this->address_,
               data_with_int3) == -1) {
      Error::SendErrno("Enabling breakpoint site failed");
    }
  }

  this->is_enabled_ = true;
}

void sdb::BreakpointSite::Disable() {
  if (!this->is_enabled_) {
    return;
  }

  if (this->is_hardware_) {
    this->process_->ClearHardwareStoppoint(this->hardware_register_index_);
    this->hardware_register_index_ = -1;
  } else {
    errno                    = 0;
    const std::uint64_t data = ptrace(PTRACE_PEEKDATA, this->process_->GetPid(),
                                      this->address_.GetAddress(), nullptr);
    if (errno != 0) {
      Error::SendErrno("Disabling breakpoint site failed");
    }

    // `ptrace` operates on whole words rather than bytes, so we need to
    // overwrite the low byte prior to writing it back to memory
    const auto restored_data =
        ((data & ~0xff) | static_cast<std::uint8_t>(saved_data_));

    // We restore the data masking the first byte with -0xff
    // and then bitwise ORing with the saved data
    if (ptrace(PTRACE_POKEDATA, this->process_->GetPid(), this->address_,
               restored_data) == -1) {
      Error::SendErrno("Disabling breakpoint site failed");
    }
  }
  this->is_enabled_ = false;
}
