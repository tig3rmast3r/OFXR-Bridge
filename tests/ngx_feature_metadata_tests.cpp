#include <d3d12.h>
#include "xrfg/ngx_feature_metadata.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct EvaluationMap {
    bool flags_present{true}, dimensions_present{};
    int flags{};
    unsigned width{1280},height{720},out_width{2560},out_height{1440};
    NVSDK_NGX_Result Get(const char* key, int* value) const {
        if (flags_present && std::strcmp(key, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) == 0) {
            *value = flags; return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char* key, unsigned* value) const {
        if (dimensions_present) {
            if (!std::strcmp(key,NVSDK_NGX_Parameter_Width)) *value=width;
            else if (!std::strcmp(key,NVSDK_NGX_Parameter_Height)) *value=height;
            else if (!std::strcmp(key,NVSDK_NGX_Parameter_OutWidth)) *value=out_width;
            else if (!std::strcmp(key,NVSDK_NGX_Parameter_OutHeight)) *value=out_height;
            else return NVSDK_NGX_Result_FAIL_InvalidParameter;
            return NVSDK_NGX_Result_Success;
        }
        *value=0xffffffffU; // Failed getters must not invent dimensions.
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
};
}
int main() {
    try {
    for(const float legacy_value:{0.F,-1.F,std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity()})
        require(xrfg::ngx_luke_frame_time_ms(legacy_value)==16.6667F,
            "legacy successful-but-invalid frame time still fails depth publication bit64");
    for(const float native_value:{8.333F,11.111F,22.222F,33.333F})
        require(xrfg::ngx_luke_frame_time_ms(native_value)==native_value,
            "positive native frame time replaced by nominal Luke interval");
        {
            xrfg::NgxFeatureMetadata a{}, b{};
            a.width=b.width=1280; a.height=b.height=720;
            a.out_width=b.out_width=2560; a.out_height=b.out_height=1440;
            require(xrfg::ngx_creator_only_twin(0x10,a,b),"exact GTA creator-only twin rejected");
            for(unsigned bit=0;bit<11;++bit) if(bit!=4)
                require(!xrfg::ngx_creator_only_twin(0x10|(1u<<bit),a,b),"additional scope mismatch admitted as twin");
            for(unsigned field=0;field<6;++field) {
                auto changed=b;
                if(field==0)changed.flags_known=false;
                if(field==1)++changed.flags;
                if(field==2)++changed.width;
                if(field==3)++changed.height;
                if(field==4)++changed.out_width;
                if(field==5)++changed.out_height;
                require(!xrfg::ngx_creator_only_twin(0x10,a,changed),"non-identical flags or dimensions admitted");
                require(!xrfg::ngx_creator_only_twin(0x10,changed,a),"asymmetric non-identical twin admitted");
            }
            a.width=b.width=0;
            require(!xrfg::ngx_creator_only_twin(0x10,a,b),"unknown dimensions admitted");
        }
        EvaluationMap map; xrfg::NgxFeatureMetadata metadata;
        map.dimensions_present=true;
        require(xrfg::ngx_display_dimension(3776,&map,NVSDK_NGX_Parameter_OutWidth,4096,true)==3776&&
            xrfg::ngx_display_dimension(3088,&map,NVSDK_NGX_Parameter_OutHeight,4096,true)==3088,
            "evaluation intermediates replaced original display dimensions");
        require(xrfg::ngx_display_dimension(0,&map,NVSDK_NGX_Parameter_OutWidth,3056,false)==3056,
            "unobserved evaluation dimension replaced actual output allocation");
        map.dimensions_present=false;
        require(xrfg::ngx_display_dimension(0,&map,NVSDK_NGX_Parameter_OutHeight,3344)==3344,
            "missing evaluation dimensions lost actual resource extent");
        require(xrfg::recover_typed_ngx_metadata(true,NVSDK_NGX_Feature_SuperSampling,&map,metadata),
            "late typed SR discarded when creation dimensions absent");
        require(metadata.flags_known && metadata.flags==0 && !metadata.width && !metadata.out_height&&
            !metadata.creation_observed,
            "zero flags mistaken for absent metadata / failed getter leaked dimensions");
        require(!xrfg::recover_typed_ngx_metadata(false,NVSDK_NGX_Feature_SuperSampling,&map,metadata),
            "generic core classified unknown feature");
        require(!xrfg::recover_typed_ngx_metadata(true,NVSDK_NGX_Feature_FrameGeneration,&map,metadata),
            "frame generation classified as SR");
        map.flags_present=false;
        require(xrfg::recover_typed_ngx_metadata(true,NVSDK_NGX_Feature_RayReconstruction,&map,metadata) &&
            !metadata.flags_known, "missing flags fabricated / typed RR producer observation suppressed");
        map.flags_present=true;map.flags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        map.dimensions_present=true;
        require(xrfg::recover_typed_ngx_metadata(true,NVSDK_NGX_Feature_RayReconstruction,&map,metadata) &&
            metadata.flags_known && metadata.flags==map.flags && !metadata.width &&
            !metadata.height && !metadata.out_width && !metadata.out_height&&!metadata.creation_observed,
            "late evaluation keys promoted into an original creation receipt");

        // Actual KCD evaluation keys reverse1672x1368 ->3344x2736 after a
        // public dispatcher enters. Only its immutable creation receipt can
        // identify display dimensions for a different typed native handle.
        map.width=3344;map.height=2736;map.out_width=1672;map.out_height=1368;
        unsigned legacy_display{};
        require(NVSDK_NGX_SUCCEED(map.Get(NVSDK_NGX_Parameter_OutWidth,&legacy_display))&&
            legacy_display==1672,"inverted native map counterfactual missing");
        const int list{},other_list{};
        xrfg::NgxFeatureMetadata created;
        created.identity=17;created.creation_source_identity=17;created.creation_observed=true;
        created.width=1672;created.height=1368;created.out_width=3344;created.out_height=2736;
        created.flags=map.flags;created.type=NVSDK_NGX_Feature_SuperSampling;
        require(!xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata),
            "creation receipt escaped its current invocation");
        {
            xrfg::NgxCreationEvaluationScope invocation(created,&list,true);
            require(xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata)&&
                metadata.creation_observed&&metadata.creation_source_identity==17&&metadata.identity==0&&
                metadata.width==1672&&metadata.height==1368&&metadata.out_width==3344&&metadata.out_height==2736,
                "same-list typed alias lost the original creation contract");
            require(xrfg::ngx_display_dimension(metadata.out_width,&map,NVSDK_NGX_Parameter_OutWidth,
                3344,metadata.creation_observed)==3344,"inverted native OutWidth replaced original display");
            require(!xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&other_list,metadata)&&
                !xrfg::NgxCreationEvaluationScope::inherit_native(created.type,nullptr,metadata)&&
                !xrfg::NgxCreationEvaluationScope::inherit_native(NVSDK_NGX_Feature_RayReconstruction,&list,metadata)&&
                !xrfg::NgxCreationEvaluationScope::inherit_native(NVSDK_NGX_Feature_FrameGeneration,&list,metadata),
                "different list/type or missing list inherited an unrelated receipt");
            {
                xrfg::NgxFeatureMetadata missed_creation;
                missed_creation.type=created.type;
                xrfg::NgxCreationEvaluationScope forwarding_alias(missed_creation,&list,true);
                require(xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata)&&
                    metadata.creation_source_identity==17&&metadata.out_width==3344,
                    "intermediate generic alias discarded its exact enclosing invocation");
            }
            xrfg::NgxCreationEvaluationScope typed_native(created,&list,false);
            require(!xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata),
                "typed recursive provider reused a public dispatch receipt");
        }
        require(!xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata),
            "finished invocation retained its creation receipt");
        auto replacement=created;replacement.identity=23;replacement.creation_source_identity=23;
        replacement.out_width=3056;replacement.out_height=3344;
        auto old_alias=created;old_alias.identity=18; // Inherited source17; not its own creation.
        {
            xrfg::NgxCreationEvaluationScope fresh_outer(replacement,&list,true);
            {
                xrfg::NgxCreationEvaluationScope forwarding_old_alias(old_alias,&list,true);
                require(xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata)&&
                    metadata.creation_source_identity==23&&metadata.out_width==3056,
                    "previously inherited generic alias blocked the current preset's exact receipt");
            }
            auto independent=created;independent.identity=41;independent.creation_source_identity=41;
            independent.out_width=4096;independent.out_height=2160;
            xrfg::NgxCreationEvaluationScope direct_creation(independent,&list,true);
            require(xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata)&&
                metadata.creation_source_identity==41&&metadata.out_width==4096,
                "enclosing unrelated receipt replaced a directly observed creation");
        }
        created.creation_observed=false;
        {
            xrfg::NgxCreationEvaluationScope unknown(created,&list,true);
            require(!xrfg::NgxCreationEvaluationScope::inherit_native(created.type,&list,metadata),
                "late evaluation masqueraded as observed creation");
        }
        require(xrfg::recover_typed_ngx_metadata(true,created.type,&map,metadata)&&
            xrfg::ngx_display_dimension(metadata.out_width,&map,NVSDK_NGX_Parameter_OutWidth,
                3344,metadata.creation_observed)==3344,"unknown late handle trusted inverted OutWidth");
        std::cout << "Typed late NGX metadata: PASS (exact creation/list/type scope, inverted eval dimensions, SR/RR/core/FG isolation)\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
