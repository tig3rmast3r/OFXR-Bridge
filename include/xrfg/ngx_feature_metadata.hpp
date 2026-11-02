#pragma once
#include <nvsdk_ngx_params.h>
#include <cstdint>

namespace xrfg {
struct NgxFeatureMetadata {
    std::uint32_t identity{};
    NVSDK_NGX_Feature type{NVSDK_NGX_Feature_SuperSampling};
    int flags{};
    unsigned width{}, height{}, out_width{}, out_height{};
    bool flags_known{true};
    bool creation_observed{};
    std::uint32_t creation_source_identity{};
};
// V772 admits an exact SR twin only when the creator is the sole failed
// scope condition. Keep every list/device/type/result/guide check in the mask.
template<class Feature, class Creator>
constexpr bool ngx_creator_only_twin(unsigned mismatch, const Feature& feature,
    const Creator& creator) noexcept {
    return mismatch == 0x10U && feature.flags_known && creator.flags_known && feature.flags == creator.flags &&
        feature.width && feature.height && feature.out_width && feature.out_height &&
        feature.width == creator.width && feature.height == creator.height &&
        feature.out_width == creator.out_width && feature.out_height == creator.out_height;
}
// Match the accepted OptiScaler publisher: feature display dimensions win.
// Evaluate maps can retain intermediate or even inverted creation-only keys.
// Only an actual creation receipt supplies a display contract; otherwise use
// the original output allocation (and the caller's actual subrect bounds).
template<class Parameters>
unsigned ngx_display_dimension(unsigned display,const Parameters*,
    const char*,unsigned resource_extent,bool creation_observed=false) {
    return creation_observed&&display?display:resource_extent;
}
// A typed implementation export proves SR/RR independently of whether its
// evaluation map still contains creation dimensions. Those keys cannot prove
// the original creation contract: capture obtains actual render subrects/resources,
// and Luke guides still require real flags and the exact bound native camera.
template<class Parameters>
bool recover_typed_ngx_metadata(bool typed, NVSDK_NGX_Feature type,
    const Parameters* parameters, NgxFeatureMetadata& metadata) {
    if (!typed || !parameters || (type != NVSDK_NGX_Feature_SuperSampling &&
        type != NVSDK_NGX_Feature_RayReconstruction)) return false;
    metadata = {}; metadata.type = type;
    metadata.flags_known = NVSDK_NGX_SUCCEED(parameters->Get(
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &metadata.flags));
    return true;
}

// A generic/public dispatcher may replace the handle or mutate parameters
// before reaching the typed implementation. Carry its immutable creation
// receipt only through that exact nested call on the same native list. This
// is invocation provenance, never a latest-feature or same-size association.
class NgxCreationEvaluationScope {
    inline static thread_local NgxCreationEvaluationScope* current_{};
    NgxCreationEvaluationScope* parent_{};
    NgxFeatureMetadata metadata_{};
    const void* native_list_identity_{};
    const void* native_device_identity_{};
    std::uint32_t public_production_identity_{};
    bool public_dispatch_{};
public:
    NgxCreationEvaluationScope(const NgxFeatureMetadata& metadata,
        const void* native_list_identity,bool public_dispatch,
        const void* native_device_identity=nullptr) noexcept
        : parent_(current_),metadata_(metadata),
          native_list_identity_(native_list_identity),native_device_identity_(native_device_identity),
          public_dispatch_(public_dispatch) {
        // Production ownership and creation geometry are separate receipts.
        // A directly created intermediate dispatcher keeps its own geometry,
        // but an exact enclosing public call still owns this production.
        if(public_dispatch_&&native_list_identity_&&native_device_identity_) {
            public_production_identity_=public_production(metadata_.type,
                native_list_identity_,native_device_identity_);
            if(!public_production_identity_&&metadata_.creation_observed&&metadata_.identity&&
                metadata_.creation_source_identity==metadata_.identity&&
                (metadata_.type==NVSDK_NGX_Feature_SuperSampling||
                 metadata_.type==NVSDK_NGX_Feature_RayReconstruction))
                public_production_identity_=metadata_.identity;
        }
        // Forward a fresh outer receipt through an intermediate generic alias
        // whose contract was inherited earlier. A directly observed creation
        // owns its own contract even inside another evaluation on that list.
        const bool inherited_alias=metadata_.creation_observed&&metadata_.creation_source_identity!=0&&
            metadata_.creation_source_identity!=metadata_.identity;
        if(public_dispatch_&&(!metadata_.creation_observed||inherited_alias)) {
            NgxFeatureMetadata inherited;
            if(inherit_native(metadata_.type,native_list_identity_,inherited))metadata_=inherited;
        }
        current_=this;
    }
    ~NgxCreationEvaluationScope() {current_=parent_;}
    NgxCreationEvaluationScope(const NgxCreationEvaluationScope&)=delete;
    NgxCreationEvaluationScope& operator=(const NgxCreationEvaluationScope&)=delete;
    static bool inherit_native(NVSDK_NGX_Feature type,const void* native_list_identity,
        NgxFeatureMetadata& metadata) noexcept {
        if(!current_||!native_list_identity||!current_->public_dispatch_||
            current_->native_list_identity_!=native_list_identity||
            !current_->metadata_.creation_observed||current_->metadata_.type!=type||
            (type!=NVSDK_NGX_Feature_SuperSampling&&type!=NVSDK_NGX_Feature_RayReconstruction))return false;
        metadata=current_->metadata_;
        metadata.identity=0; // The typed handle keeps its own generation identity.
        return true;
    }
    // This associates a production with its actual public invocation. A
    // directly created native feature keeps its own geometry; it can still be
    // one physical-eye implementation of this same public SR/RR evaluation.
    static std::uint32_t public_production(NVSDK_NGX_Feature type,
        const void* native_list_identity,const void* native_device_identity) noexcept {
        if(!native_device_identity||!current_||current_->native_device_identity_!=native_device_identity||
            !native_list_identity||current_->native_list_identity_!=native_list_identity||
            !current_->public_dispatch_||current_->metadata_.type!=type||
            (type!=NVSDK_NGX_Feature_SuperSampling&&type!=NVSDK_NGX_Feature_RayReconstruction))return 0;
        return current_->public_production_identity_;
    }
};
} // namespace xrfg
