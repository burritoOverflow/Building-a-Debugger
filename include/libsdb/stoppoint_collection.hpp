#ifndef SDB_STOPPOINT_COLLECTION_HPP
#define SDB_STOPPOINT_COLLECTION_HPP

#include <libsdb/types.hpp>
#include <memory>
#include <vector>

namespace sdb {
  template <class StopPoint>
  class StoppointCollection {
public:
    StopPoint &Push(std::unique_ptr<StopPoint> bs) {
      this->stoppoints_.push_back(std::move(bs));
      return *this->stoppoints_.back();
    }

    bool ContainsId(typename StopPoint::id_type id) const;
    bool ContainsAddress(VirtualAddress address) const;
    bool EnabledStopPointAtAddress(VirtualAddress address) const;

    StopPoint       &GetById(typename StopPoint::id_type id);
    const StopPoint &GetById(typename StopPoint::id_type id) const;

    StopPoint               &GetByAddress(VirtualAddress address);
    const StopPoint         &GetByAddress(VirtualAddress address) const;
    std::vector<StopPoint *> GetInRegion(VirtualAddress low,
                                         VirtualAddress high) const;

    void RemoveById(typename StopPoint::id_type id);
    void RemoveByAddress(VirtualAddress address);

    template <class F>
    void ForEach(F f);

    template <class F>
    void ForEach(F f) const;

    std::size_t Size() const { return this->stoppoints_.size(); }

    bool IsEmpty() const { return this->stoppoints_.empty(); }

private:
    using points_t = std::vector<std::unique_ptr<StopPoint>>;

    typename points_t::iterator       FindById(typename StopPoint::id_type id);
    typename points_t::const_iterator FindById(
        typename StopPoint::id_type id) const;
    typename points_t::iterator       FindByAddress(VirtualAddress address);
    typename points_t::const_iterator FindByAddress(
        VirtualAddress address) const;

    points_t stoppoints_;
  };

  template <class StopPoint>
  auto StoppointCollection<StopPoint>::FindById(typename StopPoint::id_type id)
      -> typename points_t::iterator {
    return std::find_if(this->stoppoints_.begin(), this->stoppoints_.end(),
                        [=](auto &point) { return point->GetId() == id; });
  }

  template <class StopPoint>
  auto StoppointCollection<StopPoint>::FindById(typename StopPoint::id_type id)
      const -> typename points_t::const_iterator {
    return const_cast<StoppointCollection *>(this)->FindById(id);
  }

  template <class StopPoint>
  auto StoppointCollection<StopPoint>::FindByAddress(VirtualAddress address) ->
      typename points_t::iterator {
    return std::find_if(this->stoppoints_.begin(), this->stoppoints_.end(),
                        [=](auto &point) { return point->AtAddress(address); });
  }

  template <class StopPoint>
  auto StoppointCollection<StopPoint>::FindByAddress(
      const VirtualAddress address) const -> typename points_t::const_iterator {
    return const_cast<StoppointCollection *>(this)->FindByAddress(address);
  }

  template <class StopPoint>
  bool StoppointCollection<StopPoint>::ContainsId(
      typename StopPoint::id_type id) const {
    return this->FindById(id) != this->stoppoints_.end();
  }

  template <class StopPoint>
  bool StoppointCollection<StopPoint>::ContainsAddress(
      const VirtualAddress address) const {
    return this->FindByAddress(address) != this->stoppoints_.end();
  }

  template <class StopPoint>
  bool StoppointCollection<StopPoint>::EnabledStopPointAtAddress(
      const VirtualAddress address) const {
    return ContainsAddress(address) && GetByAddress(address).IsEnabled();
  }

  template <class StopPoint>
  StopPoint &StoppointCollection<StopPoint>::GetById(
      typename StopPoint::id_type id) {
    auto it = this->FindById(id);
    if (it == end(this->stoppoints_)) {
      Error::Send("Invalid StopPoint id");
    }
    return **it;
  }

  template <class StopPoint>
  const StopPoint &StoppointCollection<StopPoint>::GetById(
      typename StopPoint::id_type id) const {
    return const_cast<StoppointCollection *>(this)->GetById(id);
  }

  template <class StopPoint>
  StopPoint &StoppointCollection<StopPoint>::GetByAddress(
      const VirtualAddress address) {
    auto it = this->FindByAddress(address);
    if (it == end(this->stoppoints_)) {
      Error::Send("StopPoint with given address not found");
    }
    return **it;
  }

  template <class StopPoint>
  const StopPoint &StoppointCollection<StopPoint>::GetByAddress(
      const VirtualAddress address) const {
    return const_cast<StoppointCollection *>(this)->GetByAddress(address);
  }

  template <class StopPoint>
  void StoppointCollection<StopPoint>::RemoveById(
      typename StopPoint::id_type id) {
    auto it = this->FindById(id);
    (**it).Disable();
    this->stoppoints_.erase(it);
  }

  template <class StopPoint>
  void StoppointCollection<StopPoint>::RemoveByAddress(
      const VirtualAddress address) {
    auto it = this->FindByAddress(address);
    (**it).Disable();
    this->stoppoints_.erase(it);
  }

  template <class StopPoint>
  template <class F>
  void StoppointCollection<StopPoint>::ForEach(F f) {
    for (auto &point : this->stoppoints_) {
      f(*point);
    }
  }

  template <class StopPoint>
  template <class F>
  void StoppointCollection<StopPoint>::ForEach(F f) const {
    for (const auto &point : this->stoppoints_) {
      f(*point);
    }
  }

  template <class StopPoint>
  std::vector<StopPoint *> StoppointCollection<StopPoint>::GetInRegion(
      VirtualAddress low, VirtualAddress high) const {
    std::vector<StopPoint *> ret;

    for (const auto &site : this->stoppoints_) {
      if (site->IsInRange(low, high)) {
        ret.push_back(site.get());
      }
    }

    return ret;
  }

}  // namespace sdb


#endif  // SDB_STOPPOINT_COLLECTION_HPP
