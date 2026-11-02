#pragma once

namespace xrfg {
// Generic NGX/engine/injector dispatch can surround the native SR/RR call and
// copy its output before returning. Observe at the native body when available.
// Keep generic-only implementations observable and suppress recursive aliases.
// A generic dispatch that deferred to the native body can learn, once it
// returns, that no hook inside it ran: a hook installed earlier (Cheeky
// Foveated DLSS) reached the DLSS runtime past the native body's hook.
class NgxEvaluationBoundary {
    inline static thread_local NgxEvaluationBoundary* current_{};
    NgxEvaluationBoundary* parent_{};
    unsigned slot_{};
    bool owns_{}, recursive_{}, public_guides_{}, occupied_{}, inner_entered_{}, inner_owned_{};
public:
    NgxEvaluationBoundary(unsigned slot, bool typed_native, bool native_available,
        bool public_guides = false) noexcept
        : parent_(current_), slot_(slot), public_guides_(public_guides) {
        for (auto* p = parent_; p; p = p->parent_) {
            occupied_ |= p->owns_;
            if (p->owns_ && p->slot_ == slot) p->recursive_ = true;
        }
        owns_ = (typed_native || !native_available) && !occupied_;
        for (auto* p = parent_; p; p = p->parent_) {
            p->inner_entered_ = true;
            p->inner_owned_ |= owns_;
        }
        current_ = this;
    }
    ~NgxEvaluationBoundary() { current_ = parent_; }
    NgxEvaluationBoundary(const NgxEvaluationBoundary&) = delete;
    NgxEvaluationBoundary& operator=(const NgxEvaluationBoundary&) = delete;
    bool observes() const noexcept { return owns_; }
    bool outermost() const noexcept { return parent_==nullptr; }
    bool public_guide_parent() const noexcept {
        for(auto* p=parent_;p;p=p->parent_)if(p->owns_&&p->public_guides_)return true;
        return false;
    }
    bool can_complete() const noexcept { return owns_ && !recursive_; }
    // After the call returns: whether any hook inside it ran, or observed.
    bool inner_entered() const noexcept { return inner_entered_; }
    // No hook observed this call: not this one, nor one around or inside it.
    bool unobserved() const noexcept { return !owns_ && !occupied_ && !inner_owned_; }
};
}
