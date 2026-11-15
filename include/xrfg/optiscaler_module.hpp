#pragma once
#include "xrfg/native_export_address.hpp"
#include <windows.h>
#include <string>
#include <vector>

namespace xrfg {
// Proxy filenames also belong to Windows and other injectors. Product metadata
// plus module-owned executable NGX exports identifies the implementation; a
// matching filename or a spoofed GPU vendor alone is never sufficient.
inline bool optiscaler_module_valid(HMODULE module) noexcept {
    try {
        if(!module || !GetProcAddress(module,"NVSDK_NGX_D3D12_EvaluateFeature") ||
            !native_export_address(module,"NVSDK_NGX_D3D12_CreateFeature") ||
            !native_export_address(module,"NVSDK_NGX_D3D12_EvaluateFeature") ||
            !native_export_address(module,"NVSDK_NGX_D3D12_ReleaseFeature"))return false;
        wchar_t path[32768]{};
        const auto length=GetModuleFileNameW(module,path,32768);
        if(!length || length>=32768)return false;
        DWORD unused{};const auto size=GetFileVersionInfoSizeW(path,&unused);
        if(!size || size>1024*1024)return false;
        std::vector<unsigned char> data(size);
        if(!GetFileVersionInfoW(path,0,size,data.data()))return false;
        struct Translation {WORD language,codepage;};
        Translation* translations{};UINT bytes{};
        if(!VerQueryValueW(data.data(),L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations),&bytes))return false;
        for(UINT i=0;i<bytes/sizeof(Translation);++i) {
            wchar_t key[96]{};swprintf_s(key,L"\\StringFileInfo\\%04x%04x\\ProductName",
                translations[i].language,translations[i].codepage);
            wchar_t* product{};UINT characters{};
            if(VerQueryValueW(data.data(),key,reinterpret_cast<void**>(&product),&characters) &&
                product && characters==11 && std::wstring_view(product,characters-1)==L"OptiScaler")return true;
        }
    }catch(...){}
    return false;
}

class OptiscalerEvaluationScope {
    inline static thread_local unsigned depth_{};
    inline static thread_local OptiscalerEvaluationScope* current_{};
    OptiscalerEvaluationScope* parent_{};
    void* list_{};void* output_{};unsigned dispatches_{},replacement_children_{};
    bool rejected_{},replacement_child_failed_{};
    bool replacement_{},outermost_{};
public:
    explicit OptiscalerEvaluationScope(bool replacement) noexcept :parent_(current_),replacement_(replacement) {
        current_=this;
        if(replacement_)outermost_=depth_++==0;
    }
    ~OptiscalerEvaluationScope(){
        if(outermost_)for(auto* p=parent_;p;p=p->parent_) {
            ++p->replacement_children_;p->replacement_child_failed_|=!completed();
        }
        current_=parent_;if(replacement_)--depth_;
    }
    OptiscalerEvaluationScope(const OptiscalerEvaluationScope&)=delete;
    void expect(void* list,void* output)noexcept {list_=list;output_=output;}
    static void dispatched(void* list,void* output,bool success)noexcept {
        for(auto* scope=current_;scope;scope=scope->parent_)if(scope->replacement_) {
            if(!success||!list||!output||list!=scope->list_||output!=scope->output_)scope->rejected_=true;
            else ++scope->dispatches_;
        }
    }
    bool completed()const noexcept{
        return (!replacement_||(!rejected_&&dispatches_==1)) &&
            (replacement_children_==0 || (replacement_children_==1&&!replacement_child_failed_));
    }
    static bool observing()noexcept{return depth_!=0;}
    bool active()const noexcept{return observing();}
    bool outermost()const noexcept{return outermost_;}
};
}
