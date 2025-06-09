#include <libsdb/process.hpp>
#include <libsdb/watchpoint.hpp>

namespace {
  auto GetNextId() {
    static sdb::Watchpoint::id_type id = 0;
    return ++id;
  }
}  // namespace

sdb::Watchpoint::Watchpoint(Process &process, const VirtualAddress address,
                            const StoppointMode mode, const std::size_t size) :
    process_{&process}, address_{address}, is_enabled_{false}, mode_{mode},
    size_{size} {
  // watchpoints on x64 must be aligned to their size; 8-byte watchpoints must
  // fall on 8-byte boundaries, 4-byte watchpoints on 4-byte boundaries, etc.

  /*
   *Explainer airlifted from the book:
   *
   * "If the size of the breakpoint is 8, the least significant 4 bits of the
   * address must be 0 for the address to be aligned. Because address & (8 - 1)
   * is address & 0b1111, we ensure that this calculation results in 0.
   * Similarly, if the size is 4, the least significant 3 bits of the address
   * should be 0, and address & (4 - 1) will be address & 0b111. The same
   * approach works for 2-byte and 1-byte watchpoints."
   */
  if ((address.GetAddress() & size - 1) != 0) {
    Error::Send("Watchpoints must be aligned to their size");
  }

  this->id_ = GetNextId();
}

void sdb::Watchpoint::Enable() {
  if (this->is_enabled_) {
    return;
  }

  this->hardware_register_index_ = this->process_->SetWatchpoint(
      this->id_, this->address_, this->mode_, this->size_);
  this->is_enabled_ = true;
}

void sdb::Watchpoint::Disable() {
  if (!this->is_enabled_) {
    return;
  }

  this->process_->ClearHardwareStoppoint(this->hardware_register_index_);
  this->is_enabled_ = false;
}
