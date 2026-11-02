#pragma once
#include <array>
#include <optional>
#include <utility>
#include <cstddef>
namespace xrfg {
template<class T> class ThreeSlotBuffer {
public:
    struct Entry { T value; bool synthetic; };
    void configure(unsigned outputs) noexcept { clear(); count_=outputs==4?5:outputs==3?4:3; }
    std::size_t capacity() const noexcept { return count_; }
    const std::optional<Entry>& slot(std::size_t i) const noexcept { return slots_[(head_+i)%count_]; }
    bool empty() const noexcept { for(std::size_t i=0;i<count_;++i) if(slot(i)) return false; return true; }
    bool can_accept_real() const noexcept {
        for(std::size_t i=1;i<count_;++i) if(slot(i)) return false;
        return !slot(0) || !slot(0)->synthetic;
    }
    bool push_real(T value) { if(!can_accept_real()) return false;
        slots_[(head_+count_-1)%count_].emplace(Entry{std::move(value),false}); return true; }
    bool shift_empty() noexcept { if(slot(0)||empty()) return false; head_=(head_+1)%count_; return true; }
    bool can_insert_synthetic() const noexcept {
        if(!slot(count_-2) || slot(count_-2)->synthetic || slot(count_-1)) return false;
        for(std::size_t i=0;i<count_-2;++i) if(slot(i)) return false;
        return true;
    }
    bool push_synthetic(T value) { if(count_!=3||!can_insert_synthetic()) return false;
        slots_[head_].emplace(Entry{std::move(value),true}); return true; }
    bool push_synthetic_pair(T first,T second) { if(count_!=4||!can_insert_synthetic()) return false;
        slots_[head_].emplace(Entry{std::move(first),true});
        slots_[(head_+1)%count_].emplace(Entry{std::move(second),true}); return true; }
    bool push_synthetic_triple(T first,T second,T third) { if(count_!=5||!can_insert_synthetic())return false;
        slots_[head_].emplace(Entry{std::move(first),true});
        slots_[(head_+1)%count_].emplace(Entry{std::move(second),true});
        slots_[(head_+2)%count_].emplace(Entry{std::move(third),true});return true; }
    bool releasable(bool drain=false) const noexcept {
        return slot(0) && (slot(0)->synthetic || slot(count_-1) || drain);
    }
    bool release_front(bool drain=false) noexcept { if(!releasable(drain)) return false;
        slots_[head_].reset(); head_=(head_+1)%count_; return true; }
    void clear() noexcept { slots_={}; head_=0; }
private:
    std::array<std::optional<Entry>,5> slots_{}; std::size_t head_{},count_{3};
};
}
