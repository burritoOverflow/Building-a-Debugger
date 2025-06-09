#ifndef SDB_WATCHPOINT_HPP
#define SDB_WATCHPOINT_HPP

#include <libsdb/types.hpp>

namespace sdb {
  class Process;

  class Watchpoint {
public:
    Watchpoint()                              = delete;
    Watchpoint(const Watchpoint &)            = delete;
    Watchpoint &operator=(const Watchpoint &) = delete;

    using id_type = std::int32_t;
    id_type GetId() const { return this->id_; }

    void Enable();
    void Disable();

    bool           IsEnabled() const { return this->is_enabled_; }
    VirtualAddress GetAddress() const { return this->address_; }
    StoppointMode  GetMode() const { return this->mode_; }
    std::size_t    GetSize() const { return this->size_; }

    // is the watchpoint at a given address?
    bool AtAddress(const VirtualAddress address) const {
      return address == this->address_;
    }

    // is the watchpoint within a given address range?
    bool IsInRange(const VirtualAddress low, const VirtualAddress high) const {
      return low <= this->address_ && high > this->address_;
    }

private:
    friend Process;

    Watchpoint(Process &process, VirtualAddress address, StoppointMode mode,
               std::size_t size);

    id_type        id_;
    Process       *process_;
    VirtualAddress address_;
    // selected mode for this watchpoint
    StoppointMode mode_;
    // whether the watchpoint is enabled
    bool        is_enabled_;
    std::size_t size_;
    int         hardware_register_index_ = -1;
  };
};  // namespace sdb

#endif  // SDB_WATCHPOINT_HPP
