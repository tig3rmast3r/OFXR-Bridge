#pragma once
#include "xrfg/bridge_flight_logger.hpp"
#include <openxr/openxr.h>
#include <array>
#include <bit>
#include <cstdint>
#include <type_traits>

namespace xrfg {
template<class T> inline std::uint64_t hud_handle_bits(T value) noexcept {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uint64_t>(value);
}
// CPU-only F9 receipts, emitted on a quad identity/geometry change. No image
// copy/readback, no application metadata mutation and no additional GPU work.
inline void trace_hud_geometry(const XrFrameEndInfo* frame,unsigned stage,std::uint64_t session) noexcept {
    struct Cache {std::uint64_t session{},mask{};unsigned stage{};bool known{};
        std::array<std::array<std::uint64_t,8>,8> quad{};};
    static thread_local Cache cache;
    auto& logger=bridge_flight_logger();
    if(!logger.enabled()){cache.known=false;return;}
    if(!frame || !frame->layers)return;
    const auto pair=[](std::uint32_t x,std::uint32_t y){return(std::uint64_t(x)<<32)|y;};
    const bool fresh=!cache.known || cache.session!=session || cache.stage!=stage;
    std::uint64_t mask{};
    for(unsigned i=0;i<frame->layerCount && i<8;++i) {
        const auto* header=frame->layers[i];
        if(!header || header->type!=XR_TYPE_COMPOSITION_LAYER_QUAD)continue;
        mask|=std::uint64_t(1)<<i;
        const auto& q=*reinterpret_cast<const XrCompositionLayerQuad*>(header);
        const std::array<std::uint64_t,8> shape{
            hud_handle_bits(q.subImage.swapchain),
            pair(std::bit_cast<std::uint32_t>(q.size.width),std::bit_cast<std::uint32_t>(q.size.height)),
            pair(q.subImage.imageRect.extent.width,q.subImage.imageRect.extent.height),
            pair(q.subImage.imageRect.offset.x,q.subImage.imageRect.offset.y),
            pair(std::bit_cast<std::uint32_t>(q.pose.position.z),q.subImage.imageArrayIndex),
            hud_handle_bits(q.space),std::uint64_t(q.eyeVisibility),q.layerFlags};
        if(!fresh && (cache.mask&(std::uint64_t(1)<<i)) && cache.quad[i]==shape)continue;
        cache.quad[i]=shape;
        logger.event(BridgeFlightOperation::presenter_transition,110100+stage,shape[0],shape[1],pair(i,q.eyeVisibility));
        logger.event(BridgeFlightOperation::presenter_transition,110110+stage,shape[0],shape[2],shape[3]);
        logger.event(BridgeFlightOperation::presenter_transition,110120+stage,shape[0],shape[4],shape[5]);
        logger.event(BridgeFlightOperation::presenter_transition,110140+stage,shape[0],q.layerFlags,frame->displayTime);
    }
    if(fresh || mask!=cache.mask)
        logger.event(BridgeFlightOperation::presenter_transition,110130+stage,session,frame->layerCount,mask);
    cache.session=session;cache.stage=stage;cache.mask=mask;cache.known=true;
}
}
