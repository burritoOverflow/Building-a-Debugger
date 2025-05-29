#ifndef SDB_BREAKPOINT_SITE
#define SDB_BREAKPOINT_SITE

#include <libsdb/types.hpp>

namespace sdb {
  class Process;

  class BreakpointSite {
public:
    BreakpointSite()                                  = delete;
    BreakpointSite(const BreakpointSite &)            = delete;
    BreakpointSite &operator=(const BreakpointSite &) = delete;

    using id_type = std::int32_t;

    // Breakpoint should have unique IDs, ref'd either from code or
    // on cmd line
    id_type GetId() const { return this->id_; }

    // for enabling and disabling the breakpoint site
    void Enable();
    void Disable();

    VirtualAddress Address() const { return this->address_; }

    bool AtAddress(const VirtualAddress address) const {
      return address == this->address_;
    }

    bool IsInRange(const VirtualAddress low, const VirtualAddress high) const {
      return low <= this->address_ && high > this->address_;
    }

    bool IsEnabled() const { return this->is_enabled_; }

private:
    BreakpointSite(Process &process, VirtualAddress address);
    friend Process;  // allow for access to this ctor

    id_type        id_;
    Process       *process_;
    VirtualAddress address_;
    bool           is_enabled_;
    std::byte saved_data_;  // data we replace with the int3 instruction when
                            // setting a breakpoint
  };

}  // namespace sdb

#endif  // SDB_BREAKPOINT_SITE
