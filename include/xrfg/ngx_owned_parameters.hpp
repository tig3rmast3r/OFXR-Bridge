#pragma once
#include <nvsdk_ngx_params.h>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace xrfg {
// Implements the SDK's published virtual interface for the private snippet.
// SDK Allocate/GetCapabilityParameters require shared NGX initialization; this
// map owns only OFXR values and never enters the game's NGX core or SR hooks.
class OwnedNgxParameters final:public NVSDK_NGX_Parameter {
    using Value=std::variant<unsigned long long,float,double,unsigned,int,void*>;
    struct Hash {
        using is_transparent=void;
        std::size_t operator()(std::string_view key)const noexcept{return std::hash<std::string_view>{}(key);}
    };
    std::unordered_map<std::string,Value,Hash,std::equal_to<>> values_;
    bool failed_{};
    template<class T>void set(const char* name,T value) noexcept {
        if(!name){failed_=true;return;}
        try {
            const auto found=values_.find(std::string_view(name));
            if(found!=values_.end())found->second=value;
            else values_.emplace(name,Value{value});
        }catch(...){failed_=true;}
    }
    template<class T>NVSDK_NGX_Result get(const char* name,T* value)const noexcept {
        if(!name || !value || failed_)return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto found=values_.find(std::string_view(name));
        if(found==values_.end())return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
        return std::visit([&](auto stored)->NVSDK_NGX_Result {
            using S=decltype(stored);
            if constexpr(std::is_pointer_v<T> && std::is_pointer_v<S>) {
                *value=static_cast<T>(stored);return NVSDK_NGX_Result_Success;
            } else if constexpr(std::is_arithmetic_v<T> && std::is_arithmetic_v<S>) {
                if constexpr(std::is_same_v<T,S>) {
                    *value=stored;return NVSDK_NGX_Result_Success;
                } else if constexpr(std::is_integral_v<T> && std::is_integral_v<S>) {
                    if(!std::in_range<T>(stored))return NVSDK_NGX_Result_FAIL_InvalidParameter;
                } else if constexpr(std::is_integral_v<T>) {
                    const long double n=static_cast<long double>(stored);
                    const auto upper=std::ldexp(1.0L,std::numeric_limits<T>::digits);
                    if(!std::isfinite(n) || n<static_cast<long double>(std::numeric_limits<T>::lowest()) ||
                        n>=upper)return NVSDK_NGX_Result_FAIL_InvalidParameter;
                }
                *value=static_cast<T>(stored);return NVSDK_NGX_Result_Success;
            } else return NVSDK_NGX_Result_FAIL_InvalidParameter;
        },found->second);
    }
public:
#define SCALAR(T) void Set(const char* n,T v)override{set(n,v);} NVSDK_NGX_Result Get(const char* n,T* v)const override{return get(n,v);}
    SCALAR(unsigned long long) SCALAR(float) SCALAR(double) SCALAR(unsigned int) SCALAR(int)
#undef SCALAR
#define POINTER(T) void Set(const char* n,T* v)override{set(n,static_cast<void*>(v));} NVSDK_NGX_Result Get(const char* n,T** v)const override{return get(n,v);}
    POINTER(ID3D11Resource) POINTER(ID3D12Resource) POINTER(void)
#undef POINTER
    void Reset()override{values_.clear();failed_=false;}
};
} // namespace xrfg
